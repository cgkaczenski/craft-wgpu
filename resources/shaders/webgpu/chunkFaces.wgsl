// Milestones 6-9: chunk faces, textured and unlit. The WebGPU counterpart of
// the position and albedo parts of rendering/defaultShader.vert and .frag,
// and (milestone 9) of their transparency and water peel.
//
// One instance per block face, read from the face arena with instance step
// mode: the same 16 bytes Chunk::bake writes for OpenGL (bigGpuBuffer.cpp's
// setupVertexAttributes):
//   bytes 0-1  orientation (short): which corner table row, 0-5 cube faces,
//              6-9 grass, 10-15 leaves, 22-35 water, then models
//   bytes 2-3  texture index (short): low 11 bits the texture, high 5 colors
//   bytes 4-15 block position (ivec3, world blocks); y packs light in bits
//              16-23 and flags in 24-31 (flag bit 0: water)
//
// The four corners come from vertexData, the table Renderer builds in
// recreateBlockGeometryData: 4 corners x 3 floats per orientation. OpenGL
// drew them as a triangle fan of indices 0,1,2,3; WebGPU has no fans, so a
// 4-vertex triangle strip visits them as 0,1,3,2 -- the same quad, the same
// counter-clockwise winding.
//
// Textures (milestone 7): the face's texture index picks a slot of the old
// bindless handle table (4 slots per block: albedo, normal, material,
// parallax). textureLayers maps each slot to a layer of blockTextures, one
// 128x128 texture_2d_array built by blockTextures.wgsl. UVs come from the
// same per-orientation table as GL's u_vertexUV, and need no flip: the array
// stores rows bottom first, as GL did (docs/roadmap.md, "Conventions").
//
// Transparency (milestone 9): water, ice and glass are drawn after the opaque
// faces by four pipelines that share this module (wgpuWorld.cpp has the pass
// order). fs_waterDepth writes the nearest water surface's depth; fs_blend
// draws the transparent faces with alpha blending, in the mode its pipeline's
// `passMode` override sets:
//   0  the peel: only what lies beyond the nearest water surface
//   1  the final pass, water reading the peel (what is under it)
//   2  the final pass without a peel (the game's waterType 0)
// Group 1 holds the copies the peel reads: depth and color, read texel by
// texel at the fragment's own pixel, as GL samples them at the pixel center.
//
// Decals (milestone 10a): the block-breaking crack. vs_main again, on the
// six faces of the block being broken, with the `zBias` override pulling
// them toward the camera, and fs_decal sampling one frame of crack.png.
//
// G-buffer (milestone 11): the opaque faces are drawn twice into fboMain's
// targets (four here: the roughness rides in the positions' alpha, see
// wgpuWorld.cpp's GBufferSlot). fs_depth is the depth prepass (zpass.frag: depth only, the
// alpha test still cutting leaves); fs_main then runs once per visible pixel,
// with depth compare Equal, writing GBuffer below. Both passes must compute
// the same depth for Equal to hold, hence @invariant on the position.
//
// Color: the HDR target holds linear light, as GL's does. This world is
// still unlit, so each fragment does its old math on texture colors as they
// are (sRGB) and converts the result with toLinear; wgpuWorld's composite
// converts back for the screen. Blending therefore happens in linear light,
// as in GL.
//
// Conventions (docs/roadmap.md): positions camera-relative, depth 0..1.
// No animation yet (grass, leaves sway and water waves in defaultShader.vert)
// and no lighting: albedo times the orientation shade.

struct Uniforms {
    // perspectiveRH_ZO times Camera::getViewMatrix (rotation only).
    viewProjection: mat4x4f,
    // The camera position split as in Camera::decomposePosition, the
    // u_positionInt / u_positionFloat pair. w unused.
    positionInt: vec4i,
    positionFloat: vec4f,
    // GL's u_waterColor, in output (sRGB) space: this world is unlit and
    // writes texture colors as they are, where GL mixes in linear space.
    waterColor: vec4f,
    // x near plane, y far plane: to turn depth back into distance. z the
    // crack frame, 0-10, for fs_decal (written when a decal is drawn).
    planes: vec4f,
    // Camera::getViewMatrix (rotation only): camera-relative positions to
    // view space, for the G-buffer's positions (GL's u_view).
    view: mat4x4f,
};
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read> vertexData: array<f32>;
@group(0) @binding(2) var<storage, read> vertexUV: array<f32>;
@group(0) @binding(3) var<storage, read> textureLayers: array<u32>;
@group(0) @binding(4) var blockTextures: texture_2d_array<f32>;
// Nearest, nearest mip, repeat, max LOD 4: the GL texture parameters, less
// the 4x anisotropy WebGPU only allows with linear filtering.
@group(0) @binding(5) var blockSampler: sampler;

// The peel's inputs (transparent pipelines only).
@group(1) @binding(0) var peelDepth: texture_depth_2d;
@group(1) @binding(1) var peelColor: texture_2d<f32>;

// The decal's crack.png: 10 frames side by side (milestone 10a).
@group(1) @binding(2) var crackTexture: texture_2d<f32>;

override passMode: u32 = 1u;

// zpass.vert's u_zBias, added to clip z. GL's depth is (z/w + 1)/2 and this
// one is z/w, so half the bias moves the depth by the same amount.
override zBias: f32 = 0.0;

struct FaceInput {
    @location(0) orientationAndTexture: vec2i,
    @location(1) position: vec3i,
};

struct VertexOutput {
    // @invariant: the prepass and the solid pass are different pipelines,
    // and Equal needs them to land on the same depth bit for bit.
    @invariant @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) @interpolate(flat) layer: u32,
    @location(2) shade: f32,
    // Camera-relative position, for the view direction.
    @location(3) relative: vec3f,
    @location(4) @interpolate(flat) normal: vec3f,
    @location(5) @interpolate(flat) flags: u32,
};

// defaultShader.vert's vertexColor: a fixed darkening per orientation (front,
// back, top, bottom, left, right; grass; leaves), standing in for lighting.
const vertexColor = array<f32, 16>(
    0.95, 0.85, 1.0, 0.8, 0.85, 0.95,
    0.95, 0.95, 0.95, 0.95,
    0.95, 0.85, 1.0, 0.8, 0.85, 0.95,
);

// defaultShader.vert's normals: the cube faces, in orientation order.
const faceNormals = array<vec3f, 6>(
    vec3f(0.0, 0.0, 1.0), vec3f(0.0, 0.0, -1.0), vec3f(0.0, 1.0, 0.0),
    vec3f(0.0, -1.0, 0.0), vec3f(-1.0, 0.0, 0.0), vec3f(1.0, 0.0, 0.0),
);

// defaultShader.vert's waterMask: which corners of the water orientations
// (22-27) are on the surface, and drop 0.08 of a block to sit below it.
const waterMask = array<f32, 24>(
    1.0, 1.0, 0.0, 0.0,  0.0, 1.0, 1.0, 0.0,  1.0, 1.0, 1.0, 1.0,
    0.0, 0.0, 0.0, 0.0,  0.0, 1.0, 1.0, 0.0,  1.0, 1.0, 0.0, 0.0,
);

const stripToCorner = array<u32, 4>(0u, 1u, 3u, 2u);

fn cornerShape(row: i32, corner: u32) -> vec3f {
    let base = u32(row) * 12u + corner * 3u;
    return vec3f(vertexData[base], vertexData[base + 1u], vertexData[base + 2u]);
}

// GL's linear <-> sRGB (defaultShader.frag's toLinearSRGB, and its inverse).
fn toLinear(c: vec3f) -> vec3f {
    let low = c / 12.92;
    let high = pow((c + 0.055) / 1.055, vec3f(2.4));
    return select(high, low, c < vec3f(0.04045));
}

fn fromLinear(c: vec3f) -> vec3f {
    let low = c * 12.92;
    let high = 1.055 * pow(c, vec3f(1.0 / 2.4)) - 0.055;
    return select(high, low, c < vec3f(0.0031308));
}

// defaultShader.frag's fromFloatTouShort: [-1, 1] to [0, 65536], clamped to
// 65535 (GL's 65536 for a component of exactly 1 does not fit 16 bits).
fn packNormal(n: vec3f) -> vec4u {
    return vec4u(min(vec3u((n + 1.0) * 0.5 * 65536.0), vec3u(65535u)), 0u);
}

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex: u32, face: FaceInput) -> VertexOutput {
    let orientation = face.orientationAndTexture.x;
    let textureIndex = u32(face.orientationAndTexture.y) & 0x07FFu;

    // The light and flag bytes ride in y's high bits.
    let blockPosition = vec3i(face.position.x, face.position.y & 0xFFFF, face.position.z);
    let flags = u32(face.position.y >> 24) & 0xFFu;

    // Leaves (10-15) reuse the cube faces' rows; defaultShader.vert only
    // adds their sway on top.
    var row = orientation;
    if (orientation >= 10 && orientation < 16) { row = orientation - 10; }

    let corner = stripToCorner[vertexIndex];
    var shape = cornerShape(row, corner);

    // Water's surface corners sit 0.08 below the block top (GL adds waves
    // on top of that; they come with the rest of the animation).
    if (orientation >= 22 && orientation < 28) {
        shape.y -= 0.08 * waterMask[u32(orientation - 22) * 4u + corner];
    }

    // Integer difference first, then the fraction: exact however far the
    // world is from the origin.
    let relative = vec3f(blockPosition - u.positionInt.xyz) - u.positionFloat.xyz + shape;

    var shade = 1.0;
    if (orientation >= 0 && orientation < 16) { shade = vertexColor[orientation]; }

    // defaultShader.vert's normals: the table for cube faces and water, and
    // for every other shape (grass, leaves, models) the face's own plane from
    // three of its corners, as GL takes it.
    var normal: vec3f;
    if (orientation >= 0 && orientation < 6) { normal = faceNormals[orientation]; }
    else if (orientation >= 22 && orientation < 28) { normal = faceNormals[orientation - 22]; }
    else {
        let p1 = cornerShape(row, corner);
        let p2 = cornerShape(row, (corner + 1u) % 4u);
        let p3 = cornerShape(row, (corner + 2u) % 4u);
        normal = normalize(cross(p3 - p2, p1 - p2));
    }

    // UVs are indexed by the orientation itself, leaves included
    // (defaultShader.vert's calculateUVs).
    let uvBase = u32(orientation) * 8u + corner * 2u;

    var out: VertexOutput;
    out.position = u.viewProjection * vec4f(relative, 1.0);
    out.position.z += zBias * 0.5;
    out.uv = vec2f(vertexUV[uvBase], vertexUV[uvBase + 1u]);
    out.layer = textureLayers[textureIndex]; // the albedo slot
    out.shade = shade;
    out.relative = relative;
    out.normal = normal;
    out.flags = flags;
    return out;
}

// The depth prepass (zpass.frag): no color, only the alpha test.
@fragment
fn fs_depth(in: VertexOutput) {
    let albedo = textureSample(blockTextures, blockSampler, in.uv, in.layer);
    if (albedo.a <= 0.0) { discard; }
}

// fboMain's targets, in defaultShader.frag's order, GL's fifth (the
// roughness, out_materials) in the positions' alpha.
struct GBuffer {
    @location(0) color: vec4f,      // HDR color, linear
    @location(1) position: vec4f,   // xyz view-space position (out_screenSpacePositions), w roughness
    @location(2) normal: vec4u,     // packNormal (out_normals)
    @location(3) bloom: vec4f,      // emissive light for bloom (out_bloom)
};

@fragment
fn fs_main(in: VertexOutput) -> GBuffer {
    let albedo = textureSample(blockTextures, blockSampler, in.uv, in.layer);
    // GL's alpha test: cut-out leaves, grass and flowers. After the prepass
    // this only matters with the prepass off; with it, Equal has already
    // dropped the cut-out pixels' fragments.
    if (albedo.a <= 0.0) { discard; }

    var out: GBuffer;
    out.color = vec4f(toLinear(albedo.rgb * in.shade), 1.0);
    // No material maps yet: GL's roughness without them, and no emission.
    out.position = vec4f((u.view * vec4f(in.relative, 1.0)).xyz, 0.9);
    // The face's normal: normal maps come with the lighting (milestone 12).
    out.normal = packNormal(in.normal);
    out.bloom = vec4f(0.0);
    return out;
}

fn isWater(in: VertexOutput) -> bool {
    return (in.flags & 1u) != 0u;
}

// Depth back to distance from the camera, for the 0..1 range of
// perspectiveRH_ZO (GL's linearizeDepth, less its 2d-1).
fn linearizeDepth(d: f32) -> f32 {
    let near = u.planes.x;
    let far = u.planes.y;
    return near * far / (far - d * (far - near));
}

// The water depth pass (GL's zpass shader with u_renderOnlyWater): depth only,
// so the depth target ends up holding the nearest water surface.
@fragment
fn fs_waterDepth(in: VertexOutput) {
    if (!isWater(in)) { discard; }
}

@fragment
fn fs_blend(in: VertexOutput) -> @location(0) vec4f {
    // textureSample must stay in uniform control flow (it takes derivatives
    // for the mip level), so the albedo is read before any branch that
    // depends on the pixel.
    let albedo = textureSample(blockTextures, blockSampler, in.uv, in.layer);
    let pixel = vec2i(in.position.xy);

    // The peel: GL_GREATER against the nearest water surface, by hand.
    if (passMode == 0u) {
        if (in.position.z <= textureLoad(peelDepth, pixel, 0) + 0.0000001) { discard; }
    }

    if (albedo.a <= 0.0) { discard; }

    let V = -normalize(in.relative);
    let dotNV = dot(in.normal, V);

    if (!isWater(in)) {
        // Glass and ice: GL's alpha, made more opaque at grazing angles.
        // abs: the final pass draws back faces too (no culling), and GL's
        // float target would take the >1 alpha a negative dotNV gives.
        let alpha = 1.0 - (1.0 - albedo.a) * abs(dotNV);
        return vec4f(toLinear(albedo.rgb * in.shade), alpha);
    }

    // Water: its own color, not the texture's.
    var color = u.waterColor.rgb * in.shade;
    let mixFactor = clamp(pow(1.0 - clamp(dotNV, 0.0, 1.0), 2.0) * 0.8 + 0.1, 0.0, 1.0);

    let lastDepth = linearizeDepth(textureLoad(peelDepth, pixel, 0));
    let currentDepth = linearizeDepth(in.position.z);
    let waterDepth = lastDepth - currentDepth;

    if (passMode == 1u) {
        // What is under the water, darkening with depth, mixed with the
        // surface by the view angle. GL also distorts the lookup with the
        // animated dudv map; that comes with the animation.
        // The copy is of the HDR target (linear): back to the space this
        // shader mixes in, then the result to linear again.
        var under = fromLinear(textureLoad(peelColor, pixel, 0).rgb);
        under = mix(under, u.waterColor.rgb * 0.7, 0.6 * clamp(pow(waterDepth / 18.0, 2.0), 0.0, 1.0));
        return vec4f(toLinear(mix(under, color, mixFactor)), 1.0);
    }

    // No peel: the water itself is see-through, more so looking down.
    let depthMix = 0.4 * clamp(pow(waterDepth / 18.0, 2.0), 0.0, 1.0);
    color = mix(color, u.waterColor.rgb * 0.7, depthMix);
    return vec4f(toLinear(color), pow(mixFactor, 0.7));
}

// The crack (decal.frag): frame planes.z of the strip, opaque where drawn.
// Drawn on the screen after the composite, as GL draws it after its post
// pass, so its color stays as the texture holds it.
// crack.png is a wgpu2d texture, stored top row first, where GL's gl2d
// stored it bottom row first (stbi flip on load): v is flipped to match.
@fragment
fn fs_decal(in: VertexOutput) -> @location(0) vec4f {
    let uv = vec2f((in.uv.x + u.planes.z) / 10.0, 1.0 - in.uv.y);
    let c = textureSample(crackTexture, blockSampler, uv);
    if (c.a < 0.01) { discard; }
    return vec4f(c.rgb, 1.0);
}
