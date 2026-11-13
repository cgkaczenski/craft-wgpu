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
// Reflections (milestone 17): GL's SSR, inside the lit shader as there.
// Smooth faces march their reflected ray through last frame's positions
// and take last frame's color where it lands. fs_blend now writes the
// positions and normals too (GL's transparent passes draw into all of
// fboMain but its bloom), so the AO and the next frame's reflections see
// the water's surface, not the lake bed.
//
// Animation (18-parity): defaultShader.vert's grass and leaf sway and water
// waves, in animatedCorner, which every vertex entry point goes through (the
// prepass, the solid and transparent passes and the shadow maps move alike);
// and defaultShader.frag's moving water textures: the water's normal, the
// distortion of what is under it and the caustics on what lies in it.
//
// Conventions (docs/roadmap.md): positions camera-relative, depth 0..1.

#include "world.wgsl"
#include "lighting.wgsl"

@group(0) @binding(1) var<storage, read> vertexData: array<f32>;
@group(0) @binding(2) var<storage, read> vertexUV: array<f32>;
@group(0) @binding(3) var<storage, read> textureLayers: array<u32>;
@group(0) @binding(4) var blockTextures: texture_2d_array<f32>;
// Nearest, nearest mip, repeat, max LOD 4: the GL texture parameters, less
// the 4x anisotropy WebGPU only allows with linear filtering.
@group(0) @binding(5) var blockSampler: sampler;

// 18-parity: the water's textures (GL's u_dudv, u_dudvNormal, u_caustics:
// waterDUDV.png, normal.png, caustics), tiled across the world by the
// block's xz. wgpu2d stores rows top first where gl2d stored them bottom
// first, so waterRead negates v to land on GL's texel (they repeat).
@group(0) @binding(13) var dudvTexture: texture_2d<f32>;
@group(0) @binding(14) var dudvNormalTexture: texture_2d<f32>;
@group(0) @binding(15) var causticsTexture: texture_2d<f32>;
@group(0) @binding(16) var waterSampler: sampler; // linear, mipmapped, repeat

// The frame group: the peel's inputs (transparent passes) and the sky
// behind the world, for the fog (milestone 14; GL's u_skyTexture, read at
// the pixel). Bound for the opaque and transparent lit passes.
@group(1) @binding(0) var peelDepth: texture_depth_2d;
@group(1) @binding(1) var peelColor: texture_2d<f32>;
@group(1) @binding(3) var skyColor: texture_2d<f32>;

// The decal's crack.png: 10 frames side by side (milestone 10a).
@group(1) @binding(2) var crackTexture: texture_2d<f32>;

// Milestone 17, also in the frame group: last frame's HDR color (GL's
// fboLastFrame, RGBA8: clamped to 0..1) and view-space positions
// (fboLastFramePositions), sampled as GL's FBO textures are (linear,
// clamped).
@group(1) @binding(4) var lastFrameColor: texture_2d<f32>;
@group(1) @binding(5) var lastFramePositions: texture_2d<f32>;
@group(1) @binding(6) var lastFrameSampler: sampler;

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
    // The PSX look's texture coordinates: the same uv, interpolated straight
    // across the screen, without perspective correction (`linear`), as the
    // PlayStation mapped textures. faceUV picks.
    @location(12) @interpolate(linear) affineUV: vec2f,
};

// The texture coordinates to read with: affine in the PSX look.
fn faceUV(in: VertexOutput) -> vec2f {
    return select(in.uv, in.affineUV, u.psx.w != 0.0);
}

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
// (22-35) are on the surface, where they drop 0.08 of a block and the waves
// move them.
const waterMask = array<f32, 56>(
    1.0, 1.0, 0.0, 0.0,  0.0, 1.0, 1.0, 0.0,  1.0, 1.0, 1.0, 1.0,
    0.0, 0.0, 0.0, 0.0,  0.0, 1.0, 1.0, 0.0,  1.0, 1.0, 0.0, 0.0,
    1.0, 1.0, 1.0, 1.0,  1.0, 1.0, 1.0, 1.0,  1.0, 1.0, 1.0, 1.0,  1.0, 1.0, 1.0, 1.0,
    0.0, 0.0, 1.0, 1.0,  1.0, 0.0, 0.0, 1.0,  1.0, 0.0, 0.0, 1.0,  0.0, 0.0, 1.0, 1.0,
);

// defaultShader.vert's grassMask: the corners of grass (6-9) that sway,
// its top two.
const grassMask = array<f32, 16>(
    1.0, 1.0, 0.0, 0.0,  0.0, 1.0, 1.0, 0.0,  1.0, 1.0, 0.0, 0.0,  0.0, 0.0, 1.0, 1.0,
);

const stripToCorner = array<u32, 4>(0u, 1u, 3u, 2u);

fn cornerShape(row: i32, corner: u32) -> vec3f {
    let base = u32(row) * 12u + corner * 3u;
    return vec3f(vertexData[base], vertexData[base + 1u], vertexData[base + 2u]);
}

// A textureLayers entry: the layer, and in the top byte how many blocks the
// texture spans (BlocksLoader::spannedTextures; 0 or 1 for one block).
const layerMask = 0x00FFFFFFu;

fn wrapCell(a: i32, n: i32) -> i32 { return ((a % n) + n) % n; }

// A texture across span x span blocks: which of its slices this block's face
// shows. The slices go up the texture array from the bottom row, left to
// right. Which world axes the face's u and v run along (and which way) comes
// from its corners and their UVs, so the next block along shows the next
// slice whichever way the UV table lays the face out. Cube faces only.
fn spannedLayer(entry: u32, orientation: i32, block: vec3i) -> u32 {
    let base = entry & layerMask;
    let span = i32(entry >> 24u);
    if (span < 2 || orientation < 0 || orientation >= 6) { return base; }

    let o = u32(orientation);
    let p0 = cornerShape(orientation, 0u);
    let e1 = cornerShape(orientation, 1u) - p0;
    let e2 = cornerShape(orientation, 2u) - p0;
    let t0 = vec2f(vertexUV[o * 8u], vertexUV[o * 8u + 1u]);
    let d1 = vec2f(vertexUV[o * 8u + 2u], vertexUV[o * 8u + 3u]) - t0;
    let d2 = vec2f(vertexUV[o * 8u + 4u], vertexUV[o * 8u + 5u]) - t0;
    let det = d1.x * d2.y - d2.x * d1.y;
    if (abs(det) < 1e-4) { return base; }
    let alongU = vec3i(round((e1 * d2.y - e2 * d1.y) / det));
    let alongV = vec3i(round((e2 * d1.x - e1 * d2.x) / det));

    let column = wrapCell(dot(block, alongU), span);
    let row = wrapCell(dot(block, alongV), span);
    return base + u32(column + row * span);
}

fn biasUp(a: f32) -> f32 { return (a + 0.75) / 1.75; }
fn biasUp2(a: f32) -> f32 { return ((a + 1.0) / 2.0) / 2.0 + 0.5; }

// How far each flowing-water level (1-7, from the face's shape bits 8-10)
// lowers the water's surface below a source's, in blocks.
const waterLevelDrop = 0.11;

// defaultShader.vert's calculateVertexPos: a corner of the face within its
// block, moved by the animation (u.animation.x, GL's u_timeGrass) as GL
// moves it. `facePosition` is the block's position, as floats. `waterLevel`
// is the flowing water's level, 0 for a source.
fn animatedCorner(orientation: i32, corner: u32, facePosition: vec3f, waterLevel: f32) -> vec3f {
    let t = u.animation.x;
    if (orientation >= 22 && orientation < 36) {
        // Water: its surface corners down 0.08, then two travelling waves.
        var shape = cornerShape(orientation, corner);
        let mask = waterMask[u32(orientation - 22) * 4u + corner];
        let speed = 3.1;
        let frequency = 0.5;
        let amplitude = 0.034;
        let offset = biasUp(cos((facePosition.x + shape.x + shape.z + facePosition.z - t * speed) * frequency))
            * amplitude;
        let offset2 = sin((1.0 + facePosition.x + shape.x + (shape.z + facePosition.z) * 2.0 - t * speed)
            * frequency) * amplitude;
        // Flowing water: its top corners lower by its level.
        let top = select(0.0, 1.0, shape.y > 0.5);
        shape.y += mask * (offset + offset2 - 0.08 - top * waterLevel * waterLevelDrop);
        return shape;
    }
    if (orientation >= 10 && orientation < 16) {
        // Leaves: the cube faces' rows, every corner wandering on sines.
        var shape = cornerShape(orientation - 10, corner);
        var offset = vec3f(0.0);
        offset.y = 0.01 * sin((t / 0.2) + shape.x + facePosition.x);
        offset.y += 0.04 * sin(t + shape.z + facePosition.z);
        let xz = 0.05 * sin((t / 2.0) + shape.xz + facePosition.xz)
            + 0.02 * cos((t / 3.0) + shape.xz + facePosition.xz);
        offset.y += 0.010 * cos(t + shape.x + facePosition.x);
        offset.y += 0.02 * cos((t / 2.0) + shape.z + facePosition.z);
        offset.x = xz.x;
        offset.z = xz.y;
        return shape + offset;
    }
    if (orientation >= 6 && orientation < 10) {
        // Grass: the top corners pushed along one direction by two waves.
        var shape = cornerShape(orientation, corner);
        let mask = grassMask[u32(orientation - 6) * 4u + corner];
        let dir = normalize(vec2f(0.5, 1.0));
        let dir2 = normalize(vec2f(0.9, 1.0));
        let speed = 2.0;
        let frequency = 2.5;
        let amplitude = 0.15;
        let offset = biasUp(cos((facePosition.x * dir.x + facePosition.z * dir.y - t * speed) * frequency))
            * amplitude;
        let offset2 = biasUp2(sin((facePosition.x * dir2.x + facePosition.z * dir2.y - t * speed) * frequency))
            * amplitude;
        shape.x += mask * (offset * dir.x + offset2 * dir.x);
        shape.z += mask * (offset * dir.y + offset2 * dir.y);
        return shape;
    }
    return cornerShape(orientation, corner);
}

// One corner of a face, placed by the camera: what every vertex entry point
// starts from.
fn faceVertex(vertexIndex: u32, face: FaceInput) -> VertexOutput {
    // Water faces carry the flowing water's level in bits 8-10.
    var orientation = face.orientationAndTexture.x;
    var waterLevel = 0.0;
    if (orientation >= 256) {
        waterLevel = f32((orientation >> 8u) & 7);
        orientation = orientation & 0xFF;
    }
    let textureIndex = u32(face.orientationAndTexture.y) & 0x07FFu;
    let colors = (u32(face.orientationAndTexture.y) & 0xF800u) >> 11u;

    // The light and flag bytes ride in y's high bits.
    let blockPosition = vec3i(face.position.x, face.position.y & 0xFFFF, face.position.z);
    let light = u32(face.position.y >> 16) & 0xFFu;
    let flags = u32(face.position.y >> 24) & 0xFFu;
    let water = (flags & 1u) != 0u;

    let corner = stripToCorner[vertexIndex];
    let facePosition = vec3f(blockPosition);
    let shape = animatedCorner(orientation, corner, facePosition, waterLevel);

    // Integer difference first, then the fraction: exact however far the
    // world is from the origin.
    let relative = vec3f(blockPosition - u.positionInt.xyz) - u.positionFloat.xyz + shape;

    var ambientMultiplier = 1.0;
    if (orientation >= 0 && orientation < 16 && !water) { ambientMultiplier = vertexColor[orientation]; }

    // defaultShader.vert's normals: the table for cube faces and still
    // water's six, and for every other shape (grass, leaves, flowing water,
    // models) the plane of three of its animated corners, as GL takes it.
    var normal: vec3f;
    if (orientation >= 0 && orientation < 6) { normal = faceNormals[orientation]; }
    else if (orientation >= 22 && orientation < 28) { normal = faceNormals[orientation - 22]; }
    else {
        let p2 = animatedCorner(orientation, (corner + 1u) % 4u, facePosition, waterLevel);
        let p3 = animatedCorner(orientation, (corner + 2u) % 4u, facePosition, waterLevel);
        normal = normalize(cross(p3 - p2, shape - p2));
    }

    // UVs are indexed by the orientation itself, leaves included
    // (defaultShader.vert's calculateUVs).
    let uvBase = u32(orientation) * 8u + corner * 2u;

    var out: VertexOutput;
    out.position = u.viewProjection * vec4f(relative, 1.0);
    out.position.z += zBias * 0.5;
    // The PSX look's wobble; the prepass, the lit passes and the decal all
    // come through here, so they still land on the same depth.
    out.position = psxSnap(out.position);
    out.uv = vec2f(vertexUV[uvBase], vertexUV[uvBase + 1u]);
    out.affineUV = out.uv;
    // A block's four slots: albedo, normal map, material map, parallax.
    out.layer = spannedLayer(textureLayers[textureIndex], orientation, blockPosition);
    out.normalLayer = textureLayers[textureIndex + 1u] & layerMask;
    out.materialLayer = textureLayers[textureIndex + 2u] & layerMask;
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
    let albedo = textureSample(blockTextures, blockSampler, faceUV(in), in.layer);
    if (albedo.a <= 0.0) { discard; }
}

fn isWater(in: VertexOutput) -> bool {
    return (in.flags & 1u) != 0u;
}

// ---- 18-parity: the water's moving textures --------------------------------
// defaultShader.frag's getDudvCoords family: GL's coordinates, each the
// base -blockPos.zx + uv scaled (some with v negated), plus an offset that
// moves with u.animation.y (GL's u_waterMove). The reads sit in branches
// (water, faces in water), where textureSample is not allowed, so each
// coordinate carries its screen derivatives for textureSampleGrad: the
// base's, taken in readTexels while the flow is still uniform, scaled as the
// coordinate is (GL's derivatives of the offsets that come from other reads
// are left out).

const waterSpeed = 15.0;

struct WaterCoord {
    p: vec2f,
    dx: vec2f,
    dy: vec2f,
};

fn scaledCoord(c: WaterCoord, p: vec2f, scale: vec2f) -> WaterCoord {
    var r: WaterCoord;
    r.p = p * scale;
    r.dx = c.dx * scale;
    r.dy = c.dy * scale;
    return r;
}

fn dudvCoords(base: WaterCoord, speed: f32) -> WaterCoord {
    let m = u.animation.y * speed;
    return scaledCoord(base, base.p + vec2f(m, m / 2.0), vec2f(1.0 / 20.0));
}

fn dudvCoords2(base: WaterCoord, speed: f32) -> WaterCoord {
    let m = u.animation.y * speed;
    return scaledCoord(base, base.p + vec2f(m + 3.0, m / 2.0 + 5.0), vec2f(0.1, -0.1));
}

fn dudvCoords3(base: WaterCoord, speed: f32) -> WaterCoord {
    let m = u.animation.y * speed;
    return scaledCoord(base, base.p - vec2f(m, m / 3.0), vec2f(0.1, -0.1));
}

fn dudvCoords5(base: WaterCoord, speed: f32) -> WaterCoord {
    let m = u.animation.y * speed;
    return scaledCoord(base, base.p + vec2f(m, m / 2.0), vec2f(1.0 / 50.0));
}

// GL's texture(t, c.p + shift), with v negated for wgpu2d's row order.
fn waterRead(t: texture_2d<f32>, c: WaterCoord, shift: vec2f) -> vec4f {
    let flip = vec2f(1.0, -1.0);
    return textureSampleGrad(t, waterSampler, (c.p + shift) * flip, c.dx * flip, c.dy * flip);
}

// applyNormalMap's water branch: two reads of normal.png, the first nudging
// the second, flattened to a fifth of its tilt; a texel for applyNormalMap.
fn waterNormalTexel(base: WaterCoord) -> vec3f {
    let first = waterRead(dudvNormalTexture, dudvCoords3(base, waterSpeed / 3.0), vec2f(0.0)).rg;
    let n = waterRead(dudvNormalTexture, dudvCoords5(base, waterSpeed * 2.0), first * 0.04).rgb;
    return vec3f(0.5, 0.5, 1.0) + (vec3f(n.x, n.y, 0.0) - vec3f(0.5, 0.5, 0.0)) * 0.2;
}

// The caustics on a face under water: caustics.png's channels read a little
// apart (chromatic aberration), its coordinates wobbled by two dudv reads.
fn causticsColor(base: WaterCoord) -> vec3f {
    let dudv = waterRead(dudvTexture, dudvCoords(base, waterSpeed * 2.0), vec2f(0.0)).rg
        + waterRead(dudvTexture, dudvCoords5(base, waterSpeed), vec2f(0.0)).rg;
    let c = dudvCoords(base, 5.0);
    var coords = scaledCoord(c, c.p, vec2f(3.0)); // causticsTextureScale
    coords.p += dudv / 10.0;
    let aberration = 0.004;
    let power = 0.8;
    let strength = 1.4;
    return vec3f(
        pow(waterRead(causticsTexture, coords, vec2f(0.0)).r, power),
        pow(waterRead(causticsTexture, coords, vec2f(aberration)).g, power),
        pow(waterRead(causticsTexture, coords, vec2f(aberration * 2.0)).b, power)) * strength;
}

// Every texture the lighting reads, read before anything branches on the
// pixel: textureSample takes derivatives, so it must stay in uniform
// control flow.
struct Texels {
    albedo: vec4f,
    normalMap: vec3f,
    material: vec3f,
    ao: f32,
    water: WaterCoord, // the base of the water's texture coordinates
};

fn readTexels(in: VertexOutput) -> Texels {
    var t: Texels;
    let uv = faceUV(in);
    t.albedo = textureSample(blockTextures, blockSampler, uv, in.layer);
    t.normalMap = textureSample(blockTextures, blockSampler, uv, in.normalLayer).rgb;
    t.material = textureSample(blockTextures, blockSampler, uv, in.materialLayer).rgb;
    t.ao = textureSample(aoTexture, aoSampler, aoCoordinate(in.flags, uv)).r;
    t.water.p = vec2f(-f32(in.blockPos.z), -f32(in.blockPos.x)) + uv;
    t.water.dx = dpdx(t.water.p);
    t.water.dy = dpdy(t.water.p);
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
    F0: vec3f,      // the reflectance at normal incidence
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

    let ambient = computeAmbient(withHeldLight(levels, in.relative, in.normal), in.normal, in.ambientMultiplier,
        (in.flags & 2u) != 0u, ao, shadow);

    var m = material(t.material);
    var albedo = toLinear(t.albedo.rgb);
    if (water) {
        // Water has its own color, smooth and not metal.
        albedo = u.waterColor.rgb;
        m.roughness = 0.09;
        m.metallic = 0.0;
    }
    albedo = paintAlbedo(albedo, in.colors);

    // The snow weather's cover: the tops of blocks under the open sky (the
    // sky light above them unblocked) whiten as it snows, keeping some of
    // the texture's shading, and go rough and dull.
    if (u.snowFog.w > 0.0 && !water && in.normal.y > 0.7 && levels.skyUnchanged >= 15) {
        let cover = u.snowFog.w * 0.9;
        let shading = dot(albedo, vec3f(0.2126, 0.7152, 0.0722));
        // A speckle per texel (16 to a block), so a snowy block still reads
        // as one.
        let texel = floor(faceUV(in) * 16.0) + vec2f(in.blockPos.xz) * 16.0;
        let speckle = fract(sin(dot(texel, vec2f(12.9898, 78.233))) * 43758.5453);
        let snow = vec3f(0.56, 0.58, 0.62) * (0.75 + 1.1 * sqrt(shading)) * (0.9 + 0.2 * speckle);
        albedo = mix(albedo, snow, cover);
        m.roughness = mix(m.roughness, 0.85, cover);
        m.metallic = mix(m.metallic, 0.0, cover);
    }

    // Water's normal is GL's moving normal.png (18-parity), the rest the
    // block's own normal map.
    var normalTexel = t.normalMap;
    if (water) { normalTexel = waterNormalTexel(t.water); }
    let N = applyNormalMap(in.normal, normalTexel, frontFacing);

    // The caustics: light rippling on faces under water (flag bit 1), on
    // the point lights and the sun.
    var caustics = vec3f(1.0);
    var causticsPower = 0.0;
    if ((in.flags & 2u) != 0u) {
        caustics = causticsColor(t.water);
        causticsPower = dot(caustics, vec3f(0.6, 0.3, 0.1)) * shadow;
    }

    let V = -normalize(in.relative);
    var F0 = mix(vec3f(0.04), albedo, m.metallic);
    if (water) { F0 = vec3f(0.021, 0.021, 0.05); }

    var color = pointLights(levels, in.blockPos, in.inBlock, m.metallic, m.roughness, V, albedo, N, F0, caustics);
    let sun = sunLight(levels, m.metallic, m.roughness, V, albedo, N, F0, ao, shadow, caustics);
    color += sun;

    var s: Shaded;
    s.dotNV = dot(N, V);
    // Milestone 18: under water, the color darkens toward the underwater
    // color with distance (GL's computeFogUnderWater), before the
    // reflections and the water's own mix, as in GL.
    var lit = color + albedo * ambient;
    if (u.fogParams.z != 0.0) {
        let fog = pow(exp(-pow(length(in.relative) / u.underWaterParams.x, u.underWaterParams.y)), 4.0);
        let strength = u.underWaterColor.w;
        lit = mix(u.underWaterColor.rgb, lit, vec3f(fog * strength + (1.0 - strength)));
    }
    s.alpha = 1.0 - (1.0 - t.albedo.a) * s.dotNV;
    // GL's ambient without its physically accurate reflections (switched
    // off by a constant): the albedo in the ambient light.
    s.color = lit;
    s.normal = N;
    s.roughness = m.roughness;
    s.bloom = m.emissive * 0.12 * albedo;
    // The brightest caustics glow (GL adds this to out_bloom with the sun).
    if (causticsPower > 1.2) { s.bloom += max(sun, vec3f(4.0)) * 0.0008; }
    s.F0 = F0;
    return s;
}

// ---- Milestone 17: screen-space reflections -----------------------------
// defaultShader.frag's SSR, RayMarch and BinarySearch (after Imanol
// Fotia's Epsilon Engine). Coordinates stay GL's (x * 0.5 + 0.5 with v
// running up), so its bounds and edge tests read the same; lastUv flips v
// for WebGPU's top-first textures when sampling.

const ssrMinRayStep = 1.0;
const ssrMaxSteps = 50;
const ssrBinarySearchSteps = 20;
const ssrMaxRayStep = 150.2;
const ssrMaxRayDelta = 10.0;

fn ssrProject(p: vec3f) -> vec2f {
    let c = u.projection * vec4f(p, 1.0);
    return c.xy / c.w * 0.5 + 0.5;
}

fn lastUv(coord: vec2f) -> vec2f {
    return vec2f(coord.x, 1.0 - coord.y);
}

// The view-space depth last frame had there. GL asks for mip 2 of a texture
// with one mip and a non-mipmap filter, which reads level 0.
fn lastDepthAt(coord: vec2f) -> f32 {
    return textureSampleLevel(lastFramePositions, lastFrameSampler, lastUv(coord), 0.0).z;
}

struct Ray {
    dir: vec3f,
    hit: vec3f,
    dDepth: f32,
};

// Halves the step, toward the surface, 20 times.
fn ssrBinarySearch(ray: ptr<function, Ray>, oldValue: vec2f) -> vec2f {
    var found = oldValue;
    for (var i = 0; i < ssrBinarySearchSteps; i++) {
        let coord = ssrProject((*ray).hit);
        let depth = lastDepthAt(coord);
        if (depth < -1000.0) { continue; }
        found = coord;
        (*ray).dDepth = (*ray).hit.z - depth;
        (*ray).dir *= 0.5;
        if ((*ray).dDepth > 0.0) { (*ray).hit += (*ray).dir; } else { (*ray).hit -= (*ray).dir; }
    }
    let coord = ssrProject((*ray).hit);
    if (!(lastDepthAt(coord) < -1000.0)) { found = coord; }
    return found;
}

struct RayHit {
    coord: vec2f,
    hit: bool,
};

// Steps along `dir` (longer the more the surface faces the eye) until the
// ray is behind what last frame drew, then searches back for the surface.
fn ssrRayMarch(dir: vec3f, start: vec3f, worldNormal: vec3f, viewDir: vec3f) -> RayHit {
    var ray: Ray;
    ray.dir = dir * mix(ssrMinRayStep, ssrMaxRayStep, abs(dot(worldNormal, viewDir)));
    ray.hit = start;
    ray.dDepth = 0.0;
    var result: RayHit;
    result.coord = vec2f(0.0);
    result.hit = false;
    for (var i = 0; i < ssrMaxSteps; i++) {
        ray.hit += ray.dir;
        let coord = ssrProject(ray.hit);
        if (coord.x > 1.0 || coord.y > 1.0 || coord.x < -1.0 || coord.y < -1.0) { break; }
        let depth = lastDepthAt(coord);
        if (depth > 1000.0) { continue; }
        if (depth < -1000.0) { continue; }
        ray.dDepth = ray.hit.z - depth;
        if ((ray.dir.z - ray.dDepth) < ssrMaxRayStep && ray.dDepth <= 0.0) {
            let found = ssrBinarySearch(&ray, coord);
            if (ray.dDepth < -ssrMaxRayDelta) { break; }
            if (lastDepthAt(found) < -10000.0) { break; }
            result.coord = found;
            result.hit = true;
            break;
        }
    }
    return result;
}

// GL's murmurHash33 and hash33: three random floats in 0..1 from a position's bits.
fn murmurHash33(source: vec3u) -> vec3u {
    let M = 0x5bd1e995u;
    var src = source;
    var h = vec3u(1190494759u, 2147483647u, 3559788179u);
    src *= M;
    src ^= src >> vec3u(24u);
    src *= M;
    h *= M;
    h ^= vec3u(src.x);
    h *= M;
    h ^= vec3u(src.y);
    h *= M;
    h ^= vec3u(src.z);
    h ^= h >> vec3u(13u);
    h *= M;
    h ^= h >> vec3u(15u);
    return h;
}

fn hash33(source: vec3f) -> vec3f {
    let h = murmurHash33(bitcast<vec3u>(source));
    return bitcast<vec3f>((h & vec3u(0x007fffffu)) | vec3u(0x3f800000u)) - 1.0;
}

// The ray's jitter, toward the normal's side, larger the rougher.
fn computeJitter(wp: vec3f, viewNormal: vec3f, roughness: f32) -> vec3f {
    var random = hash33(wp);
    if (dot(random, viewNormal) <= 0.0) { random = -random; }
    return (random * 2.0 - 1.0) * (roughness * 0.2);
}

struct Reflection {
    color: vec3f,
    found: bool,
};

// GL's SSR(): the color last frame had where the reflected ray lands, or
// not found (off screen, toward the camera, faded out at the screen's edge).
fn ssr(viewPos: vec3f, N: vec3f, roughness: f32, wp: vec3f, viewDir: vec3f, viewSpaceNormal: vec3f) -> Reflection {
    var r: Reflection;
    r.color = vec3f(0.0);
    r.found = false;
    let reflected = normalize(reflect(normalize(viewPos), viewSpaceNormal));
    if (reflected.z > 0.0) { return r; }
    let jitter = computeJitter(wp, viewSpaceNormal, roughness);
    let march = ssrRayMarch(normalize((jitter + reflected) * max(ssrMinRayStep, -viewPos.z)), viewPos, N, viewDir);
    if (!march.hit) { return r; }
    let dCoords = smoothstep(vec2f(0.2), vec2f(0.6), abs(vec2f(0.5) - march.coord));
    let screenEdgeFactor = clamp(1.0 - (dCoords.x + dCoords.y), 0.0, 1.0);
    if (screenEdgeFactor * -reflected.z <= 0.001) { return r; }
    r.color = textureSampleLevel(lastFrameColor, lastFrameSampler, lastUv(march.coord), 0.0).rgb;
    r.found = true;
    return r;
}

fn fresnelSchlickRoughness(cosTheta: f32, F0: vec3f, roughness: f32) -> vec3f {
    return F0 + (max(vec3f(1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// defaultShader.frag's "ssr" block, with the SSR setting on, on faces
// smoother than 0.45. GL reads the face's view position from last frame's
// positions at the pixel; this uses the face's own, which is the same
// while the camera is still and right while it moves. Water takes the
// reflection whole (its mix with what is under it comes after); anything
// else mixes it in by its fresnel and roughness. The ray steps by the
// face's own normal for water (GL's v_normal), the mapped one otherwise.
fn reflectSurface(in: VertexOutput, s: Shaded) -> Reflection {
    var r: Reflection;
    r.color = s.color;
    r.found = false;
    if (u.lightParams.w == 0 || s.roughness >= 0.45) { return r; }
    let water = isWater(in);
    let viewPos = (u.view * vec4f(in.relative, 1.0)).xyz;
    let viewSpaceNormal = normalize((u.view * vec4f(s.normal, 0.0)).xyz);
    var stepNormal = s.normal;
    if (water) { stepNormal = in.normal; }
    let V = -normalize(in.relative);
    let found = ssr(viewPos, stepNormal, s.roughness * s.roughness, in.relative, V, viewSpaceNormal);
    if (!found.found) { return r; }
    r.found = true;
    if (water) {
        r.color = found.color;
        return r;
    }
    let fresnel = fresnelSchlickRoughness(s.dotNV, s.F0, s.roughness);
    r.color = mix(mix(found.color, s.color, fresnel), s.color, pow(s.roughness, 0.5));
    return r;
}

// defaultShader.frag's computeFog (after Sildur's shaders, as GL notes):
// how much of the lit color is left at `dist` blocks from the camera, the
// rest going to the sky behind. A near haze thickening with distance, more
// so low down (airDensity grows with height above 60), and a hard edge at
// u_fogDistance so the world's end fades into the sky. GL's rainStrength was
// a constant 0 there; milestone G1 feeds it from the weather (fogParams.w):
// rain thins the density a little, flattens the height falloff and raises
// the haze. GL's time-of-day term (tmult) was commented out, its rain end
// kept here: tmult = rainStrength. GL divides by the near fog's density, so
// its setting of 0 ("disables" the near fog, by the menu) makes a NaN; here 0
// skips it.
fn computeFog(distance: f32, fragmentHeight: f32) -> f32 {
    let rain = u.fogParams.w;
    let density = u.fogParams.y * (1.0 - rain * 0.115);
    let airDensity = max(fragmentHeight / 10.0, 6.0);
    let eyeBrightnessSmooth = 120.0;

    var near = 1.0;
    if (density > 0.0) {
        let d = pow(distance * 0.8, 0.85);
        let height = mix(airDensity, 6.0, rain);
        let tmult = rain;
        let haze = (2.625 + rain * 3.4) / exp(-60.0 / 10.0 / density) * exp(-airDensity / density)
            * (1.0 - exp(-pow(d, 2.712) * height / density / (6000.0 - tmult * tmult * 2000.0) / 13.0)) / height;
        near = 1.0 - pow(clamp(haze, 0.0, 1.0), 1.0 - rain * 0.63)
            * clamp((eyeBrightnessSmooth / 255.0 - 2.0 / 16.0) * 4.0, 0.0, 1.0);
    }

    let far = pow(exp(-pow(distance / u.fogParams.x, 32.0)), 0.10);
    return far * far * near;
}

// GL's last step: the color toward the sky behind it, above water; then
// the snow weather's whiteout.
fn applyFog(in: VertexOutput, color: vec3f) -> vec3f {
    if (u.fogParams.z != 0.0) { return color; }
    let sky = textureLoad(skyColor, vec2i(in.position.xy), 0).rgb;
    let fogged = mix(sky, color, computeFog(length(in.relative), f32(in.blockPos.y) + in.inBlock.y));
    return applySnowFog(fogged, in.relative, in.normal, lightLevels(in.light).skyUnchanged);
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
    let lit = reflectSurface(in, s).color;
    var out: GBuffer;
    out.color = vec4f(applyFog(in, lit), 1.0);
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

// What the transparent passes write: the color, blended; the positions
// (roughness in w) and normals over what was there, as GL's (blending is
// on for its color target only); bloom's target masked (GL turns its draw
// buffer off for these passes), though WebGPU still wants the value.
struct BlendOutput {
    @location(0) color: vec4f,
    @location(1) position: vec4f,
    @location(2) normal: vec4u,
    @location(3) bloom: vec4f,
};

fn blendOutput(in: VertexOutput, s: Shaded, color: vec4f) -> BlendOutput {
    var out: BlendOutput;
    out.color = color;
    out.position = vec4f((u.view * vec4f(in.relative, 1.0)).xyz, s.roughness);
    out.normal = packNormal(s.normal);
    out.bloom = vec4f(0.0);
    return out;
}

@fragment
fn fs_blend(in: VertexOutput, @builtin(front_facing) frontFacing: bool) -> BlendOutput {
    let t = readTexels(in);
    let pixel = vec2i(in.position.xy);

    // The peel: GL_GREATER against the nearest water surface, by hand.
    if (passMode == 0u) {
        if (in.position.z <= textureLoad(peelDepth, pixel, 0) + 0.0000001) { discard; }
    }

    if (t.albedo.a <= 0.0) { discard; }

    let s = shade(in, t, frontFacing);
    let reflection = reflectSurface(in, s);

    // Glass and ice: lit, with GL's alpha (more opaque at grazing angles).
    if (!isWater(in)) { return blendOutput(in, s, vec4f(applyFog(in, reflection.color), s.alpha)); }

    // Water: the lit surface mixed, by the view angle, with what is under it.
    let mixFactor = clamp(pow(1.0 - clamp(s.dotNV, 0.0, 1.0), 2.0) * 0.8 + 0.1, 0.0, 1.0);

    let lastDepth = linearizeDepth(textureLoad(peelDepth, pixel, 0));
    let currentDepth = linearizeDepth(in.position.z);
    let waterDepth = lastDepth - currentDepth;

    if (passMode == 1u) {
        // What is under the water, its lookup wobbled by the moving dudv
        // map (18-parity), darkening with depth. GL's distortion is in its
        // coordinates (v up), so v's part goes the other way here. Where the
        // wobbled point lands on something in front of the water, the
        // straight lookup is kept.
        var dudv = waterRead(dudvTexture, dudvCoords(t.water, waterSpeed), vec2f(0.0)).rg;
        dudv += waterRead(dudvTexture, dudvCoords2(t.water, dudv.x), vec2f(0.0)).rg * 0.01;
        let distortion = (dudv * 2.0 - 1.0) * 0.009;
        let size = vec2f(textureDimensions(peelColor));
        let distortedUv = in.position.xy / size + vec2f(distortion.x, -distortion.y);
        let distortedPixel = clamp(vec2i(distortedUv * size), vec2i(0), vec2i(size) - 1);
        let distortDepth = linearizeDepth(textureLoad(peelDepth, distortedPixel, 0));

        var under = textureLoad(peelColor, pixel, 0).rgb;
        var finalDepth = waterDepth;
        if (distortDepth >= currentDepth + 0.1) {
            under = textureSampleLevel(peelColor, lastFrameSampler, distortedUv, 0.0).rgb;
            finalDepth = distortDepth - currentDepth;
        }
        under = mix(under, u.waterColor.rgb * 0.7, 0.6 * clamp(pow(finalDepth / 18.0, 2.0), 0.0, 1.0));
        return blendOutput(in, s, vec4f(applyFog(in, mix(under, reflection.color, mixFactor)), 1.0));
    }

    // No peel: the water itself is see-through, more so looking down; less
    // darkened where it reflects.
    let darken = select(0.4, 0.2, reflection.found);
    let depthMix = darken * clamp(pow(waterDepth / 18.0, 2.0), 0.0, 1.0);
    return blendOutput(in, s, vec4f(applyFog(in, mix(reflection.color, u.waterColor.rgb * 0.7, depthMix)),
        pow(mixFactor, 0.7)));
}

// The crack (decal.frag): frame planes.z of the strip, opaque where drawn.
// Drawn on the screen after the composite, as GL draws it after its post
// pass, so its color stays as the texture holds it.
// crack.png is a wgpu2d texture, stored top row first, where GL's gl2d
// stored it bottom row first (stbi flip on load): v is flipped to match.
@fragment
fn fs_decal(in: VertexOutput) -> @location(0) vec4f {
    let faceUv = faceUV(in);
    let uv = vec2f((faceUv.x + u.planes.z) / 10.0, 1.0 - faceUv.y);
    let c = textureSample(crackTexture, blockSampler, uv);
    if (c.a < 0.01) { discard; }
    return vec4f(c.rgb, 1.0);
}
