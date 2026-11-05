// Milestones 6-12: chunk faces. The WebGPU counterpart of
// rendering/defaultShader.vert and .frag: positions and albedo (6-8), their
// transparency and water peel (9), the G-buffer (11) and the lighting (12).
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
// wgpuWorld.cpp's GBufferSlot). fs_depth is the depth prepass (zpass.frag:
// depth only, the alpha test still cutting leaves); fs_main then runs once
// per visible pixel, with depth compare Equal, writing GBuffer below. Both
// passes must compute the same depth for Equal to hold, hence @invariant on
// the position.
//
// Lighting (milestone 12): lighting.wgsl, GL's lit path. fs_main and
// fs_blend both shade with it, into the HDR target's linear light; the
// composite tone maps it for the screen.
//
// Conventions (docs/roadmap.md): positions camera-relative, depth 0..1.
// No animation yet (grass, leaves sway and water waves in defaultShader.vert).

#include "world.wgsl"
#include "lighting.wgsl"

@group(0) @binding(1) var<storage, read> vertexData: array<f32>;
@group(0) @binding(2) var<storage, read> vertexUV: array<f32>;
@group(0) @binding(3) var<storage, read> textureLayers: array<u32>;
@group(0) @binding(4) var blockTextures: texture_2d_array<f32>;
// Nearest, nearest mip, repeat, max LOD 4: the GL texture parameters, less
// the 4x anisotropy WebGPU only allows with linear filtering.
@group(0) @binding(5) var blockSampler: sampler;

// The frame group: the peel's inputs (transparent passes) and the sky
// behind the world, for the fog (milestone 14; GL's u_skyTexture, read at
// the pixel). Bound for the opaque and transparent lit passes.
@group(1) @binding(0) var peelDepth: texture_depth_2d;
@group(1) @binding(1) var peelColor: texture_2d<f32>;
@group(1) @binding(3) var skyColor: texture_2d<f32>;

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
    // The face's albedo, normal map and material map layers (GL's
    // v_textureSampler, v_normalSampler, v_materialSampler).
    @location(1) @interpolate(flat) layer: u32,
    @location(6) @interpolate(flat) normalLayer: u32,
    @location(7) @interpolate(flat) materialLayer: u32,
    // GL's v_ambientMultiplier: the orientation's fixed darkening.
    @location(2) @interpolate(flat) ambientMultiplier: f32,
    // Camera-relative position, for the view direction.
    @location(3) relative: vec3f,
    @location(4) @interpolate(flat) normal: vec3f,
    // Flag bits: 0 water, 1 in water, 4-7 the AO shape.
    @location(5) @interpolate(flat) flags: u32,
    // The block's position and the point within it (GL's fragmentPositionI
    // and fragmentPositionF), for the point lights.
    @location(8) @interpolate(flat) blockPos: vec3i,
    // (Not named `fragment`: a reserved word in Metal Shading Language,
    // which wgpu translates this to without renaming struct members.)
    @location(9) inBlock: vec3f,
    // The baked light byte: sky light high nibble, block light low.
    @location(10) @interpolate(flat) light: u32,
    // The paint (the texture index's high 5 bits), 0 for none.
    @location(11) @interpolate(flat) colors: u32,
};

// defaultShader.vert's vertexColor: a fixed darkening per orientation (front,
// back, top, bottom, left, right; grass; leaves).
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

// One corner of a face, placed by the camera: what every vertex entry point
// starts from.
fn faceVertex(vertexIndex: u32, face: FaceInput) -> VertexOutput {
    let orientation = face.orientationAndTexture.x;
    let textureIndex = u32(face.orientationAndTexture.y) & 0x07FFu;
    let colors = (u32(face.orientationAndTexture.y) & 0xF800u) >> 11u;

    // The light and flag bytes ride in y's high bits.
    let blockPosition = vec3i(face.position.x, face.position.y & 0xFFFF, face.position.z);
    let light = u32(face.position.y >> 16) & 0xFFu;
    let flags = u32(face.position.y >> 24) & 0xFFu;
    let water = (flags & 1u) != 0u;

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

    var ambientMultiplier = 1.0;
    if (orientation >= 0 && orientation < 16 && !water) { ambientMultiplier = vertexColor[orientation]; }

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
    // A block's four slots: albedo, normal map, material map, parallax.
    out.layer = textureLayers[textureIndex];
    out.normalLayer = textureLayers[textureIndex + 1u];
    out.materialLayer = textureLayers[textureIndex + 2u];
    out.ambientMultiplier = ambientMultiplier;
    out.relative = relative;
    out.normal = normal;
    out.flags = flags;
    out.blockPos = blockPosition;
    out.inBlock = shape;
    out.light = light;
    out.colors = colors;
    return out;
}

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex: u32, face: FaceInput) -> VertexOutput {
    return faceVertex(vertexIndex, face);
}

// Milestone 13: the sun's shadow maps (renderShadow, zpass.vert with the
// cascade's matrix). The same corner, placed by a cascade's matrix instead
// of the camera's; fs_depth's alpha test lets leaves and grass cast their
// cut-out shapes. Each cascade is its own pipeline, picked by this override.
override shadowCascade: u32 = 0u;

@vertex
fn vs_shadow(@builtin(vertex_index) vertexIndex: u32, face: FaceInput) -> VertexOutput {
    var out = faceVertex(vertexIndex, face);
    out.position = u.shadowMatrices[shadowCascade] * vec4f(out.relative, 1.0);
    return out;
}

// The depth prepass (zpass.frag): no color, only the alpha test.
@fragment
fn fs_depth(in: VertexOutput) {
    let albedo = textureSample(blockTextures, blockSampler, in.uv, in.layer);
    if (albedo.a <= 0.0) { discard; }
}

fn isWater(in: VertexOutput) -> bool {
    return (in.flags & 1u) != 0u;
}

// Every texture the lighting reads, read before anything branches on the
// pixel: textureSample takes derivatives, so it must stay in uniform
// control flow.
struct Texels {
    albedo: vec4f,
    normalMap: vec3f,
    material: vec3f,
    ao: f32,
};

fn readTexels(in: VertexOutput) -> Texels {
    var t: Texels;
    t.albedo = textureSample(blockTextures, blockSampler, in.uv, in.layer);
    t.normalMap = textureSample(blockTextures, blockSampler, in.uv, in.normalLayer).rgb;
    t.material = textureSample(blockTextures, blockSampler, in.uv, in.materialLayer).rgb;
    t.ao = textureSample(aoTexture, aoSampler, aoCoordinate(in.flags, in.uv)).r;
    return t;
}

// A face lit: what fs_main and fs_blend both start from.
struct Shaded {
    color: vec3f,   // linear light
    alpha: f32,     // the texture's, made more opaque at grazing angles
    normal: vec3f,  // the normal-mapped normal, toward the eye
    roughness: f32,
    bloom: vec3f,   // the emissive light for bloom
    dotNV: f32,
};

// defaultShader.frag's lit path (u_shaders on), fog aside.
fn shade(in: VertexOutput, t: Texels, frontFacing: bool) -> Shaded {
    let levels = lightLevels(in.light);
    let water = isWater(in);
    let ao = aoFromTexel(in.flags, t.ao);

    // The sun's shadow, with the face's own normal (GL's v_normal) and the
    // distance ahead of the camera that picks the cascade.
    let viewDepth = -(u.view * vec4f(in.relative, 1.0)).z;
    let shadow = sunShadow(in.relative, viewDepth, dot(u.sunDirection.xyz, in.normal), in.position.xy);

    let ambient = computeAmbient(levels, in.normal, in.ambientMultiplier, (in.flags & 2u) != 0u, ao, shadow);

    var m = material(t.material);
    var albedo = toLinear(t.albedo.rgb);
    if (water) {
        // Water has its own color, smooth and not metal.
        albedo = u.waterColor.rgb;
        m.roughness = 0.09;
        m.metallic = 0.0;
    }
    albedo = paintAlbedo(albedo, in.colors);

    // Water's normal is GL's animated dudv map, which comes with the water
    // animation; until then the surface is flat.
    var N = applyNormalMap(in.normal, t.normalMap, frontFacing);
    if (water) { N = select(-in.normal, in.normal, frontFacing); }

    let V = -normalize(in.relative);
    var F0 = mix(vec3f(0.04), albedo, m.metallic);
    if (water) { F0 = vec3f(0.021, 0.021, 0.05); }

    var color = pointLights(levels, in.blockPos, in.inBlock, m.metallic, m.roughness, V, albedo, N, F0);
    color += sunLight(levels, m.metallic, m.roughness, V, albedo, N, F0, ao, shadow);

    var s: Shaded;
    s.dotNV = dot(N, V);
    s.alpha = 1.0 - (1.0 - t.albedo.a) * s.dotNV;
    // GL's ambient without its physically accurate reflections (switched
    // off by a constant): the albedo in the ambient light.
    s.color = color + albedo * ambient;
    s.normal = N;
    s.roughness = m.roughness;
    s.bloom = m.emissive * 0.12 * albedo;
    return s;
}

// defaultShader.frag's computeFog (after Sildur's shaders, as GL notes):
// how much of the lit color is left at `dist` blocks from the camera, the
// rest going to the sky behind. A near haze thickening with distance, more
// so low down (airDensity grows with height above 60), and a hard edge at
// u_fogDistance so the world's end fades into the sky. GL's rain and
// time-of-day terms are constants there (rainStrength 0, tmult 0) and are
// folded in. GL divides by the near fog's density, so its setting of 0
// ("disables" the near fog, by the menu) makes a NaN; here 0 skips it.
fn computeFog(distance: f32, fragmentHeight: f32) -> f32 {
    let density = u.fogParams.y;
    let airDensity = max(fragmentHeight / 10.0, 6.0);
    let eyeBrightnessSmooth = 120.0;

    var near = 1.0;
    if (density > 0.0) {
        let d = pow(distance * 0.8, 0.85);
        let height = airDensity;
        let haze = 2.625 / exp(-60.0 / 10.0 / density) * exp(-airDensity / density)
            * (1.0 - exp(-pow(d, 2.712) * height / density / 6000.0 / 13.0)) / height;
        near = 1.0 - clamp(haze, 0.0, 1.0) * clamp((eyeBrightnessSmooth / 255.0 - 2.0 / 16.0) * 4.0, 0.0, 1.0);
    }

    let far = pow(exp(-pow(distance / u.fogParams.x, 32.0)), 0.10);
    return far * far * near;
}

// GL's last step: the color toward the sky behind it, above water.
fn applyFog(in: VertexOutput, color: vec3f) -> vec3f {
    if (u.fogParams.z != 0.0) { return color; }
    let sky = textureLoad(skyColor, vec2i(in.position.xy), 0).rgb;
    return mix(sky, color, computeFog(length(in.relative), f32(in.blockPos.y) + in.inBlock.y));
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
fn fs_main(in: VertexOutput, @builtin(front_facing) frontFacing: bool) -> GBuffer {
    let t = readTexels(in);
    // GL's alpha test: cut-out leaves, grass and flowers. After the prepass
    // this only matters with the prepass off; with it, Equal has already
    // dropped the cut-out pixels' fragments.
    if (t.albedo.a <= 0.0) { discard; }

    let s = shade(in, t, frontFacing);
    var out: GBuffer;
    out.color = vec4f(applyFog(in, s.color), 1.0);
    out.position = vec4f((u.view * vec4f(in.relative, 1.0)).xyz, s.roughness);
    out.normal = packNormal(s.normal);
    out.bloom = vec4f(s.bloom, 0.0);
    return out;
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
fn fs_blend(in: VertexOutput, @builtin(front_facing) frontFacing: bool) -> @location(0) vec4f {
    let t = readTexels(in);
    let pixel = vec2i(in.position.xy);

    // The peel: GL_GREATER against the nearest water surface, by hand.
    if (passMode == 0u) {
        if (in.position.z <= textureLoad(peelDepth, pixel, 0) + 0.0000001) { discard; }
    }

    if (t.albedo.a <= 0.0) { discard; }

    let s = shade(in, t, frontFacing);

    // Glass and ice: lit, with GL's alpha (more opaque at grazing angles).
    if (!isWater(in)) { return vec4f(applyFog(in, s.color), s.alpha); }

    // Water: the lit surface mixed, by the view angle, with what is under it.
    let mixFactor = clamp(pow(1.0 - clamp(s.dotNV, 0.0, 1.0), 2.0) * 0.8 + 0.1, 0.0, 1.0);

    let lastDepth = linearizeDepth(textureLoad(peelDepth, pixel, 0));
    let currentDepth = linearizeDepth(in.position.z);
    let waterDepth = lastDepth - currentDepth;

    if (passMode == 1u) {
        // What is under the water, darkening with depth. GL also distorts
        // the lookup with the animated dudv map; that comes with the
        // animation.
        var under = textureLoad(peelColor, pixel, 0).rgb;
        under = mix(under, u.waterColor.rgb * 0.7, 0.6 * clamp(pow(waterDepth / 18.0, 2.0), 0.0, 1.0));
        return vec4f(applyFog(in, mix(under, s.color, mixFactor)), 1.0);
    }

    // No peel: the water itself is see-through, more so looking down.
    let depthMix = 0.4 * clamp(pow(waterDepth / 18.0, 2.0), 0.0, 1.0);
    return vec4f(applyFog(in, mix(s.color, u.waterColor.rgb * 0.7, depthMix)), pow(mixFactor, 0.7));
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
