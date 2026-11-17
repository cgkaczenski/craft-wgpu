// Milestones 6-12: chunk faces. The WebGPU counterpart of
// rendering/defaultShader.vert and .frag: positions and albedo (6-8), their
// transparency and water peel (9), the G-buffer (11) and the lighting (12).
//
// One instance per block face, read from the face arena with instance step
// mode: the same 16 bytes Chunk::bake writes for OpenGL (bigGpuBuffer.cpp's
// setupVertexAttributes):
//   bytes 0-1  orientation (short): low 13 bits the corner table row, 0-5
//              cube faces, 6-9 grass, 10-15 leaves, 22-35 water, then
//              models; bits 13-15 flowing water's level (water faces only)
//   bytes 2-3  texture index (short): low 11 bits the texture, high 5 colors
//              (on water, which way it runs: waterFlowWaves)
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
// Animation (18-parity): defaultShader.vert's grass and leaf sway, in
// animatedCorner, which every vertex entry point goes through (the
// prepass, the solid and transparent passes and the shadow maps move alike);
// and defaultShader.frag's moving water textures: the caustics on what lies
// in water, and the normal of water's sides.
//
// Water (no longer GL's): its surface moves and tilts by a sum of waves
// (waterWaves), and its colour is a mirror of the sky and the world by
// Fresnel's share over what is under it, dimmed by depth (waterColour).
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
    // Flag bits: 0 water, 1 in water, 4-7 the AO shape; 8 the top of a cube
    // (not in the face's data: faceVertex sets it), where puddles lie; 16-23
    // on water's surface, the solid blocks around it (shoreFoam).
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
    // x: 0 for a block's face; for a grass blade (vs_blade), 1 plus how far
    // up the blade, 0 at the root to 1 at the tip. For a blade, y is its
    // own tint, z how much of it has given way to its sprite, 0..1, and w
    // which blade it is (bladeDissolved); for a face, y is how much of a
    // tall-grass sprite has given way to blades, 0..1 (spriteDissolved).
    @location(13) blade: vec4f,
    // For a shell layer (vs_shell): x its height among the layers, above 0
    // to under 1, y which layer it is, zw the place on the block's top, 0..1
    // each way (the demo's uv). 0 for everything else.
    @location(14) shell: vec4f,
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

const topOfCubeFlag = 256u;
// Flag bit 2 (Chunk::bake): a plain tall-grass plant's sprite, which gives
// way to blades near the camera (vs_blade).
const tallGrassFlag = 4u;
// Flag bit 3 (Chunk::bake): ground grass blades stand on, the top of a grass
// block open to the air or of anything under tall grass (bladeGround). Under
// tall grass the position's y also has bit 15, which arrives as
// tuftAboveFlag (past the flags byte, as topOfCubeFlag is).
const bladeGroundFlag = 8u;
const tuftAboveFlag = 512u;
// A plant's crossed sprite (tall grass, a flower, a cobweb: orientations
// 6-9), set here, past the flags byte: it takes the sun as the ground does
// (sunFacing).
const plantSpriteFlag = 1024u;

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

// How far each flowing-water level (1-7) lowers the water's surface below a
// source's, in blocks.
const waterLevelDrop = 0.11;

// How far a corner of water's surface lies below a source's surface. Each
// corner's height is the average of the water around it (Chunk::bake's
// waterCorners), 4 bits a corner (corner x + 2 z): 0-14 half levels down,
// 15 up at the block's top (water stands over a block beside it). `corner`
// is in the block, about its middle (-0.5 to 0.5).
fn waterCornerDrop(surface: u32, corner: vec3f) -> f32 {
    let k = select(0u, 1u, corner.x > 0.0) + select(0u, 2u, corner.z > 0.0);
    let code = (surface >> (4u * k)) & 0xFu;
    if (code == 15u) { return -0.08; }
    return f32(code) * waterLevelDrop * 0.5;
}

// The tilt of water's surface from its corners: its normal, waves aside.
fn waterSurfaceNormal(surface: u32) -> vec3f {
    let d0 = waterCornerDrop(surface, vec3f(-0.5, 0.5, -0.5));
    let d1 = waterCornerDrop(surface, vec3f(0.5, 0.5, -0.5));
    let d2 = waterCornerDrop(surface, vec3f(-0.5, 0.5, 0.5));
    let d3 = waterCornerDrop(surface, vec3f(0.5, 0.5, 0.5));
    // Heights are minus the drops: rising along x and z by these.
    let riseX = ((d0 + d2) - (d1 + d3)) * 0.5;
    let riseZ = ((d0 + d1) - (d2 + d3)) * 0.5;
    return normalize(vec3f(-riseX, 1.0, -riseZ));
}

// ---- The water's waves ----------------------------------------------------
// The surface as a sum of travelling waves, each a sine sharpened into a
// crest (exp(sin x - 1): narrow tops, wide flat troughs), from 26 blocks
// long to a quarter of a block (after the fbm water of Garrett Gunnell's
// Water repository, itself after GPU Gems' sum of sines). Each wave pushes
// the ones after it along its own direction, toward its crest, which gathers
// the small waves on the fronts of the large ones.
//
// The waves' vectors are whole numbers of turns across 1024 blocks and their
// rates whole numbers of turns in 1024 seconds, so the sea is the same
// every 1024 blocks and seconds: positions and the clock (u.animation.z)
// are wrapped to that and the floats stay exact however far from the origin
// or long the game has run. The rates are deep water's (the square root of
// g times the wave number, longer waves faster), slowed to three quarters.
//
// The long waves are low (blocks' shores are a block high) and the short
// ones as steep as waves get. The first waveVertexCount move the surface's
// corners (animatedCorner); all of them tilt its normal (fs_blend).
const waveCount = 12;
const waveVertexCount = 4;
const waveTile = 1024.0;
const waveVector = array<vec2f, 12>(
    vec2f(0.2208932, 0.1043107),
    vec2f(0.1902136, 0.3190680),
    vec2f(0.5706409, -0.0184078),
    vec2f(0.1533981, 0.8590292),
    vec2f(1.2333206, -0.5215535),
    vec2f(1.3805827, 1.4848934),
    vec2f(2.1659809, -2.1659809),
    vec2f(-1.2026409, 4.4976317),
    vec2f(6.8722339, -1.2149128),
    vec2f(-7.4060592, 7.4060592),
    vec2f(5.3750687, -14.7630311),
    vec2f(-23.1815177, 2.0309906),
);
const waveNumber = array<f32, 12>(0.244284, 0.371464, 0.570938, 0.872618, 1.339066, 2.027540, 3.063160,
    4.655646, 6.978797, 10.473749, 15.711093, 23.270318);
const waveRate = array<f32, 12>(1.1596895, 1.4296701, 1.7732818, 2.1966605, 2.7182140, 3.3440781, 4.1110685,
    5.0682725, 6.2034183, 7.6024088, 9.3081954, 11.3330501);
const waveAmplitude = array<f32, 12>(0.049123, 0.050000, 0.050000, 0.050000, 0.050000, 0.050000, 0.050000,
    0.036515, 0.024359, 0.013796, 0.007818, 0.004486);
// exp(sin x - 1) on average, taken off so the waves leave the water's level
// where it was.
const waveMean = 0.4658;
// How far a wave pushes the ones after it, in its own heights.
const waveDrag = 1.6;

// The wind raises them (u.grass.y: 0.35 in clear weather, 1 in a storm).
fn waveStrength() -> f32 {
    return mix(0.85, 1.9, clamp((u.grass.y - 0.35) / 0.65, 0.0, 1.0));
}

struct Waves {
    height: f32,   // above the still surface, in blocks
    slope: vec2f,  // the height's change along x and z
    lost: f32,     // the slopes' variance of the waves too small to show
};

// The first `count` waves at `p` (a world xz, wrapped to waveTile).
// `footprint` is how much of the world a pixel covers there, in blocks: a
// wave less than four pixels long fades out (it would only flicker) and its
// tilt goes into `lost`, which the surface takes as roughness, so far water
// keeps the sun's glitter as a wide sheen.
fn waterWaves(p: vec2f, count: i32, footprint: f32) -> Waves {
    var w: Waves;
    w.height = 0.0;
    w.slope = vec2f(0.0);
    w.lost = 0.0;
    let strength = waveStrength();
    var q = p;
    for (var i = 0; i < count; i++) {
        let k = waveVector[i];
        let number = waveNumber[i];
        let amplitude = waveAmplitude[i] * strength;
        let shown = 1.0 - smoothstep(0.25, 0.5, footprint * number / (2.0 * PI));
        let x = dot(k, q) - waveRate[i] * u.animation.z;
        let crest = exp(sin(x) - 1.0);
        let change = amplitude * crest * cos(x); // of the height, per radian
        w.height += amplitude * (crest - waveMean) * shown;
        w.slope += k * change * shown;
        let tilt = amplitude * number * 0.3;
        w.lost += tilt * tilt * (1.0 - shown * shown);
        q -= k / number * change * waveDrag;
    }
    return w;
}

// ---- Running water ---------------------------------------------------------
// Water that runs (down the slope of its surface) says which way in its
// face's colour bits (Chunk::bake): 1-8 one of eight directions, and 9 on
// a side where it falls (water over it, or an edge it pours over).

const waterFalling = 9u;

// The direction of a flow code 1-8, along x and z (each -1, 0 or 1).
fn flowDirection(code: u32) -> vec2i {
    let cell = i32(select(code, code - 1u, code <= 4u));
    return vec2i(cell % 3 - 1, cell / 3 - 1);
}

// The waves of running water at `p` (a world xz, wrapped to waveTile): three
// short, steep waves that travel the way it runs, each a little to one side
// of it, over the still water's own (which are halved there). Their vectors
// are whole numbers of turns across the waves' tile whichever of the eight
// ways they are turned, and their rates whole turns in its time, as the
// others' are. Returns the surface's slope.
const flowWaveTurns = array<vec2i, 3>(vec2i(700, 90), vec2i(1100, -170), vec2i(1750, 260));
const flowWaveRates = array<f32, 3>(1600.0, 2304.0, 3392.0);
const flowWaveAmplitude = array<f32, 3>(0.030, 0.018, 0.010);

fn waterFlowWaves(p: vec2f, code: u32, footprint: f32) -> vec2f {
    let d = flowDirection(code);
    // Running along a diagonal the vectors come out longer by the square
    // root of two (they are still whole numbers); the rates go up to match
    // (by 45/32, which keeps them whole) and the heights down.
    let diagonal = d.x != 0 && d.y != 0;
    let stretch = select(1.0, 1.40625, diagonal);
    var slope = vec2f(0.0);
    for (var i = 0; i < 3; i++) {
        let turns = flowWaveTurns[i];
        let k = vec2f(vec2i(turns.x * d.x - turns.y * d.y, turns.x * d.y + turns.y * d.x)) * (2.0 * PI / waveTile);
        let number = length(k);
        let shown = 1.0 - smoothstep(0.25, 0.5, footprint * number / (2.0 * PI));
        let amplitude = flowWaveAmplitude[i] / stretch;
        let x = dot(k, p) - flowWaveRates[i] * stretch * (2.0 * PI / waveTile) * u.animation.z;
        slope += k * (amplitude * exp(sin(x) - 1.0) * cos(x) * shown);
    }
    return slope;
}

// A fall: on a side of water, streaks of foam running down it. How much
// foam at a pixel, 0..1, and the cell of the foam's grid it is in (the
// grid lies on the face: along it and up it).
struct FallFoam {
    amount: f32,
    cell: vec2i,
};

fn fallFoam(in: VertexOutput, footprint: f32) -> FallFoam {
    var f: FallFoam;
    // Along the face: z on a face that looks along x, else x.
    let alongX = abs(in.normal.x) < 0.5;
    let along = select(f32(in.blockPos.z & 1023) + in.inBlock.z, f32(in.blockPos.x & 1023) + in.inBlock.x, alongX);
    let up = f32(in.blockPos.y) + in.inBlock.y;
    f.cell = vec2i(floor(vec2f(along + 0.5, up + 0.5) * f32(foamCells)));
    // The streaks: a noise six cells wide and twenty-four tall, falling
    // three blocks a second, with a finer one over it. (The clock wraps
    // every 1024 seconds; 1024 * 3 * 32 cells is a whole number.)
    let fallen = i32(floor(u.animation.z * 3.0 * f32(foamCells)));
    let streak = vec2i(f.cell.x * 4, f.cell.y + fallen);
    let noise = 0.65 * groundNoise(streak, 24, 31) + 0.35 * groundNoise(vec2i(streak.x * 2, streak.y), 11, 32);
    f.amount = 0.85 * smoothstep(0.5, 0.82, mix(noise, 0.55, smoothstep(0.02, 0.08, footprint)));
    return f;
}

// Running water's surface: streaks of foam drawn out the way it runs (its
// flow code 1-8), a few cells across and many long, carried downstream a
// block and a half a second. On the surface's own grid of foam cells
// (foamCell), so it dithers as the rest of the foam does; along and across
// are whole numbers of cells whichever of the eight ways it runs. How much
// foam at a pixel, 0..1. (The clock wraps every 1024 seconds; 1024 * 1.5 *
// 32 cells is a whole number.)
fn flowFoam(in: VertexOutput, footprint: f32) -> f32 {
    let d = flowDirection(in.colors);
    let cell = foamCell(in);
    let along = cell.x * d.x + cell.y * d.y;
    let across = cell.y * d.x - cell.x * d.y;
    let carried = i32(floor(u.animation.z * 1.5 * f32(foamCells)));
    // Six cells across, thirty-six long, with a finer one over it.
    let streak = vec2i(across * 6, along - carried);
    let noise = 0.65 * groundNoise(streak, 36, 33) + 0.35 * groundNoise(vec2i(streak.x * 2, streak.y), 15, 34);
    return 0.75 * smoothstep(0.54, 0.8, mix(noise, 0.5, smoothstep(0.02, 0.08, footprint)));
}

// A world xz wrapped to the waves' tile.
fn waveCoordinate(p: vec2f) -> vec2f {
    return p - waveTile * floor(p / waveTile);
}

// defaultShader.vert's calculateVertexPos: a corner of the face within its
// block, moved by the animation (u.animation.x, GL's u_timeGrass) as GL
// moves it. `facePosition` is the block's position, as floats. `surface`
// is water's surface, its corners' heights (waterCornerDrop), 0 for a source.
fn animatedCorner(orientation: i32, corner: u32, facePosition: vec3f, surface: u32) -> vec3f {
    let t = u.animation.x;
    if (orientation >= 22 && orientation < 36) {
        // Water: its surface corners down 0.08, then the long waves
        // (GL moved them by two sines).
        var shape = cornerShape(orientation, corner);
        let mask = waterMask[u32(orientation - 22) * 4u + corner];
        let waves = waterWaves(waveCoordinate(facePosition.xz + shape.xz), waveVertexCount, 0.0);
        // Flowing water: its top corners lower, each as the water around it.
        // (The corners are about the block's middle: its top is at 0.5.)
        let top = select(0.0, 1.0, shape.y > 0.0);
        shape.y += mask * (waves.height - 0.08 - top * waterCornerDrop(surface, shape));
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
fn faceVertex(vertexIndex: u32, face: FaceInput, forShadow: bool) -> VertexOutput {
    // The orientation: the corner table has over 5000 rows (the models), so
    // the low 13 bits. The attribute is a signed short: read its 16 bits
    // unsigned.
    let water = (u32(face.position.y >> 24) & 1u) != 0u;
    let orientationBits = u32(face.orientationAndTexture.x) & 0xFFFFu;
    var orientation = i32(orientationBits & 0x1FFFu);
    let textureIndex = u32(face.orientationAndTexture.y) & 0x07FFu;
    var colors = (u32(face.orientationAndTexture.y) & 0xF800u) >> 11u;
    // Water's orientations are all under 128, and its surface's corners
    // (waterCornerDrop) ride above them: shape bits 7-15 hold the low 9
    // bits, the flag byte's bits 2-7 the next 6 and the colour's top bit
    // (its flow code needs 4) the last.
    var surface = 0u;
    if (water) {
        orientation = i32(orientationBits & 0x7Fu);
        surface = (orientationBits >> 7u) | (((u32(face.position.y) >> 26u) & 0x3Fu) << 9u)
            | ((colors >> 4u) << 15u);
        colors = colors & 0xFu;
    }

    // The light and flag bytes ride in y's high bits.
    // y is 0-255. Above it, bit 15 is a flag (tuftAboveFlag); on water's
    // surface bits 8-15 are its shore (shoreFoam), handed on in the flags'
    // bits 16-23.
    let blockPosition = vec3i(face.position.x, face.position.y & 0xFF, face.position.z);
    let light = u32(face.position.y >> 16) & 0xFFu;
    let aboveY = (u32(face.position.y) >> 8u) & 0xFFu;
    // (On water only the flag byte's two low bits are flags.)
    let flags = (u32(face.position.y >> 24) & select(0xFFu, 0x3u, water))
        | select(select(0u, tuftAboveFlag, (aboveY & 0x80u) != 0u), aboveY << 16u, water);

    let corner = stripToCorner[vertexIndex];
    let facePosition = vec3f(blockPosition);
    var shape = animatedCorner(orientation, corner, facePosition, surface);

    // A tall-grass sprite where blades grow instead: toward the camera it
    // loses its texels one by one (spriteDissolved) while its tuft's blades
    // come in one by one (vs_blade, by the same distance), so neither is
    // seen to shrink or grow. Wholly gone, it is no more than a point. The
    // same in the nearest of the sun's shadow maps, where the blades cast
    // their own shadows (vs_bladeShadow); in the farther two the sprite
    // stays, and its shadow is the tuft's.
    // Toward the end of that cascade the sprite comes back into it as the
    // blades' shadows go (bladeShadowShare).
    var dissolve = 0.0;
    if ((flags & tallGrassFlag) != 0u && u.grass.x > 0.0 && (!forShadow || shadowCascade == 0u)) {
        let away = length(vec2f(blockPosition.xz - u.positionInt.xz) - u.positionFloat.xz);
        var spriteShare = bladeSpriteShare(away);
        if (forShadow) { spriteShare = max(spriteShare, 1.0 - bladeShadowShare(away)); }
        dissolve = 1.0 - spriteShare;
        if (dissolve >= 1.0) { shape = vec3f(0.0, -0.5, 0.0); }
    }

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
    // Water's surface tilts as its corners slope (still water's lies flat).
    else if (orientation == 24) { normal = waterSurfaceNormal(surface); }
    else if (orientation >= 22 && orientation < 28) { normal = faceNormals[orientation - 22]; }
    else {
        let p2 = animatedCorner(orientation, (corner + 1u) % 4u, facePosition, surface);
        let p3 = animatedCorner(orientation, (corner + 2u) % 4u, facePosition, surface);
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
    out.flags = flags | select(0u, topOfCubeFlag, orientation == 2)
        | select(0u, plantSpriteFlag, orientation >= 6 && orientation < 10);
    out.blockPos = blockPosition;
    out.inBlock = shape;
    out.light = light;
    out.colors = colors;
    out.blade = vec4f(0.0, dissolve, 0.0, 0.0);
    return out;
}

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex: u32, face: FaceInput) -> VertexOutput {
    return faceVertex(vertexIndex, face, false);
}

// ---- Grass blades -----------------------------------------------------------
// Real blades of grass near the camera: tufts where tall grass grows (whose
// sprites give way to them here, faceVertex), and here and there a small
// clump of short grass on a grass block's bare top. One instance per
// cell, a block the blades stand in (Renderer's grassCellsInView), packed as:
//   xyz  the block, in world blocks (the blades stand on its floor, y - 0.5)
//   w    bits 0-7 the light byte, 8-18 the texture index of the tall
//        grass's sprite (each blade takes a texel of it), 19 a tuft,
//        20-21 its band of distance (0 near, 2 far)
// A cell's blades are one strip, each blade's first and last vertex doubled
// to join it to the next with triangles of no area. A blade is
// bladeBandSegments segments, narrowing to a point; where it stands, its
// height, width, turn, lean and texel come from its cell and number, so it
// is the same every frame.
//
// What makes a field of them read as grass rather than spikes:
//  - Many blades: bladeTuftCount in a tuft, u.grass.w in a clump, spread
//    evenly, and as many at every distance.
//  - Patches: a noise across the world makes the grass taller and greener
//    here, with more clumps; shorter and drier there, with few (bladeNoise).
//  - Colour and light as the sprite's, so the two can't be told apart
//    where one gives way to the other: each blade is one texel of the
//    sprite's own picture, lit with a sprite face's normal, and its colour
//    from root to tip follows the picture's rows (bladeAlbedo).
//  - Wind: gusts of noise rolling downwind, bending a blade more toward its
//    tip, and a flutter of its own.
// It is lit by fs_main like a block's face. Toward the edge of u.grass.x
// the blades thin out a texel-high piece at a time (bladeDissolved) as the
// sprite's texels come back (faceVertex); a clump, which has no sprite,
// thins out there too.

@group(2) @binding(0) var<storage, read> grassCells: array<vec4i>;

// A tuft's blades at most, and its blade's segments in each band; a clump's
// blade's segments (wgpuWorld's grassTuftBlades, grassTuftSegments and
// grassClumpSegments: keep them alike). u.grass.w is a clump's blades at
// most.
const bladeTuftCount = 32.0;
const bladeTuftSegments = array<u32, 4>(4u, 3u, 2u, 2u);
const bladeClumpSegments = 3u;
const bladeHidden = vec4f(0.0, 0.0, 2.0, 1.0); // beyond the far plane: clipped

fn bladeHash(p: vec3i, i: u32) -> vec4f {
    var h = bitcast<u32>(p.x) * 0x8da6b343u ^ bitcast<u32>(p.y) * 0xcb1ab31fu ^ bitcast<u32>(p.z) * 0xd8163841u
        ^ i * 0x9e3779b9u;
    var r: vec4f;
    for (var k = 0; k < 4; k++) {
        h = (h ^ (h >> 16u)) * 0x7feb352du;
        h = (h ^ (h >> 15u)) * 0x846ca68bu;
        h = h ^ (h >> 16u);
        r[k] = f32(h >> 8u) / 16777216.0;
    }
    return r;
}

// Smooth noise across the world, 0..1: a value at every whole point, eased
// between them.
fn bladeLattice(p: vec2i) -> f32 {
    var h = bitcast<u32>(p.x) * 0x8da6b343u ^ bitcast<u32>(p.y) * 0xd8163841u;
    h = (h ^ (h >> 16u)) * 0x7feb352du;
    h = (h ^ (h >> 15u)) * 0x846ca68bu;
    return f32((h ^ (h >> 16u)) >> 8u) / 16777216.0;
}
fn bladeNoise(p: vec2f) -> f32 {
    let cell = floor(p);
    let f = p - cell;
    let e = f * f * (3.0 - 2.0 * f);
    let c = vec2i(cell);
    return mix(mix(bladeLattice(c), bladeLattice(c + vec2i(1, 0)), e.x),
        mix(bladeLattice(c + vec2i(0, 1)), bladeLattice(c + vec2i(1, 1)), e.x), e.y);
}

// The patches, 0..1 across the world.
fn bladePatch(world: vec2f) -> f32 {
    return 0.65 * bladeNoise(world * 0.19) + 0.35 * bladeNoise(world * 0.53 + vec2f(31.7, 11.3));
}

// Where the blades give way to the tall grass's sprites, toward the edge of
// the reach: how much of the sprite shows `away` blocks from the camera, 0
// to 1. The blades show as much less. Every blade is there at every
// distance short of that: a cell never gains or loses blades as the camera
// moves, which is seen as grass growing and dying back (Renderer's
// grassCellsInView draws the same count in every band).
fn bladeSpriteShare(away: f32) -> f32 {
    return smoothstep(u.grass.x * 0.62, u.grass.x * 0.95, away);
}

// A blade giving way to its sprite: whether this piece of it has gone yet.
// A piece is a texel high (16 to a block, the sprite's own grain), and each
// has its own turn, the same on every frame: the blade is not seen to
// shrink or to vanish at once, it thins out as the sprite's texels fill in
// (spriteDissolved). in.blade.z is how much has gone, in.blade.w the blade.
fn bladeDissolved(in: VertexOutput) -> bool {
    if (in.blade.x <= 0.0 || in.blade.z <= 0.0) { return false; }
    let piece = vec2f(floor((in.inBlock.y + 0.5) * 16.0), floor(in.blade.w * 1024.0));
    return fract(sin(dot(piece, vec2f(12.9898, 78.233))) * 43758.5453) < in.blade.z;
}

// A tall-grass sprite giving way to blades: whether this texel of it has
// gone yet. Each texel has its own turn, the same on every frame.
fn spriteDissolved(in: VertexOutput) -> bool {
    if (in.blade.x != 0.0 || in.blade.y <= 0.0) { return false; }
    let texel = floor(faceUV(in) * 16.0) + vec2f(in.blockPos.xz) * 17.0 + f32(in.blockPos.y) * 5.0;
    return fract(sin(dot(texel, vec2f(12.9898, 78.233))) * 43758.5453) < in.blade.y;
}

// The ground under the blades: how much of it their roots and the shade of
// them cover, 0..1, where a tuft stands (most of its block) and where a
// clump does (vs_blade's own clump, worked out the same way), a little
// more in the lush patches. Shown as a dither (bladeGroundShaded), so it is
// whole texels, darker or not, as the held light's bands are.
fn bladeGroundCover(in: VertexOutput) -> f32 {
    if ((in.flags & bladeGroundFlag) == 0u || u.grass.x <= 0.0 || u.grass.z <= 0.0) { return 0.0; }
    // The blades' cell: the block above this top.
    let cell = in.blockPos + vec3i(0, 1, 0);
    let cellRelative = vec3f(cell - u.positionInt.xyz) - u.positionFloat.xyz;
    let shown = 1.0 - bladeSpriteShare(length(cellRelative.xz));
    if (shown <= 0.0) { return 0.0; }

    let at = in.inBlock.xz;
    let cellLush = smoothstep(0.25, 0.75, bladePatch(vec2f(cell.xz)));
    var cover = 0.0;
    if ((in.flags & tuftAboveFlag) != 0u) {
        cover = 0.85 * (1.0 - smoothstep(0.12, 0.55, length(at)));
    } else if (u.grass.w > 0.0) {
        let clump = bladeHash(cell, 6007u);
        if (clump.z <= mix(0.12, 0.7, cellLush)) {
            let clumpSize = mix(0.6, 1.0, clump.w);
            let centre = (clump.xy - 0.5) * 0.55;
            let reach = 0.32 * clumpSize;
            cover = 0.75 * (1.0 - smoothstep(reach * 0.3, reach, length(at - centre)))
                * min(u.grass.w / 16.0, 1.0);
        }
    }
    return clamp(cover * mix(0.8, 1.0, cellLush), 0.0, 1.0) * shown * min(u.grass.z * 1.5, 1.0);
}

// Whether this texel of the ground is one of the shaded ones: the cover
// against a 4x4 Bayer matrix laid over the top's own texels (16 a block),
// so the pattern is painted on the ground and stays put as the camera moves.
fn bladeGroundShaded(in: VertexOutput) -> bool {
    let cover = bladeGroundCover(in);
    if (cover <= 0.0) { return false; }
    let texel = vec2i(floor((vec2f(in.blockPos.xz) + in.inBlock.xz + 0.5) * 16.0));
    return bayer4(texel) < cover;
}

// How dark a shaded texel of the ground under the blades is.
const bladeGroundShade = 0.5;

// A blade's colour: the sprite's picture read up the blade's own column, the
// row at the blade's height in the block, so a tuft is dark low down and
// light higher up just where its sprite is, in the same texel-high steps.
// Where that texel is see-through (the blade stands taller than the picture
// there), the blade's own texel, which never is (vs_blade).
fn bladeAlbedo(in: VertexOutput, own: vec4f) -> vec4f {
    // The picture's v runs up, as a sprite's faces map it (0 at its foot).
    let row = (floor(clamp(in.inBlock.y + 0.5, 0.0, 0.999) * 16.0) + 0.5) / 16.0;
    let pick = textureSampleLevel(blockTextures, blockSampler, vec2f(in.uv.x, row), in.layer, 0.0);
    if (pick.a > 0.5) { return pick; }
    return own;
}

@vertex
fn vs_blade(@builtin(vertex_index) vertexIndex: u32, @builtin(instance_index) instance: u32) -> VertexOutput {
    return bladeVertex(vertexIndex, instance, false);
}

// The blades into the nearest cascade of the sun's shadow maps (wgpuWorld's
// bladeShadowPipeline): the same blades, placed by the cascade's matrix, so
// the ground under them and anything beside them takes their shadow. The
// tall-grass sprites give way to them there as on screen (faceVertex), so a
// tuft's shadow is the blades', not the sprite's as well. Not the farther
// cascades: a blade is less than a texel of them. The blades take no
// shadow of their own (shade asks from above their block).
@vertex
fn vs_bladeShadow(@builtin(vertex_index) vertexIndex: u32, @builtin(instance_index) instance: u32) -> VertexOutput {
    var out = bladeVertex(vertexIndex, instance, true);
    if (out.blade.x <= 0.0) { return out; }
    out.position = u.shadowMatrices[0] * vec4f(out.relative, 1.0);
    // Fewer of them toward the cascade's end, piece by piece as they give
    // way to the sprites (bladeDissolved, fs_bladeDepth).
    let cellRelative = vec3f(out.blockPos - u.positionInt.xyz) - u.positionFloat.xyz;
    out.blade.z = max(out.blade.z, 1.0 - bladeShadowShare(length(cellRelative.xz)));
    return out;
}

// How much of a tuft's shadow, in the nearest cascade, is its blades' (the
// rest its sprite's), by how far the cell is from the camera: all of it
// near, none by where that cascade starts to mix with the next
// (lighting.wgsl's sunShadow). The next has only the sprites, so the
// ground's shadows are alike where one gives way to the other; with the
// blades' shadows to the end, the ground was darker inside a line some 15
// blocks ahead, and the line moved with the camera.
fn bladeShadowShare(away: f32) -> f32 {
    let end = shadowCascadeEnds.x - shadowCascadeBlend.x;
    return 1.0 - smoothstep(end - 6.0, end - 0.5, away);
}

// `forShadow`: for the sun's shadow map, where a blade keeps its own turn
// (not pulled toward the camera, or every shadow would turn as the player
// walked).
fn bladeVertex(vertexIndex: u32, instance: u32, forShadow: bool) -> VertexOutput {
    var out: VertexOutput;
    out.position = bladeHidden;

    let cell = grassCells[instance];
    let packed = u32(cell.w);
    let light = packed & 0xFFu;
    let textureIndex = (packed >> 8u) & 0x7FFu;
    let tuft = ((packed >> 19u) & 1u) != 0u;
    let segments = select(bladeClumpSegments, bladeTuftSegments[(packed >> 20u) & 3u], tuft);

    // Which blade of the cell, and which vertex of its strip.
    let perBlade = 2u * segments + 3u;
    let number = vertexIndex / perBlade;
    let corner = u32(clamp(i32(vertexIndex % perBlade) - 1, 0, i32(2u * segments)));

    let seed = number + select(0u, 7919u, tuft);
    let r = bladeHash(cell.xyz, seed);
    let r2 = bladeHash(cell.xyz, seed + 977u);
    let cellShift = bladeHash(cell.xyz, select(4099u, 5003u, tuft));

    // The patches: lusher grass here, poorer there. For the cell as a whole
    // (whether it has a clump) and, below, for each blade.
    let cellLush = smoothstep(0.25, 0.75, bladePatch(vec2f(cell.xz)));
    // A clump: on some of the bare grass blocks, more of them in the lush
    // patches; its own place in the block and its own size.
    let clump = bladeHash(cell.xyz, 6007u);
    if (!tuft && clump.z > mix(0.12, 0.7, cellLush)) { return out; }
    let clumpSize = mix(0.6, 1.0, clump.w);

    // Where in the block it stands: the next point of a sequence that fills
    // its patch evenly however many of them show; about the middle for a
    // tuft, about the clump's own middle for a clump.
    let spot = fract(cellShift.xy + f32(number) * vec2f(0.7548776662, 0.5698402910)) - 0.5;
    let jitter = select((clump.xy - 0.5) * 0.55 + spot * (0.22 + 0.2 * r.x) * clumpSize,
        spot * (0.7 + 0.25 * r.x), tuft);
    let cellRelative = vec3f(cell.xyz - u.positionInt.xyz) - u.positionFloat.xyz;
    let base = cellRelative + vec3f(jitter.x, -0.5, jitter.y);
    let world = vec2f(cell.xz) + jitter;

    let radius = u.grass.x;
    let away = length(base.xz);
    if (radius <= 0.0 || away >= radius) { return out; }

    let lush = smoothstep(0.25, 0.75, bladePatch(world));

    // As many blades at any distance (the density setting's share of them).
    // How much of each has gone to the sprite, by how far the middle of the
    // block is, so it changes as one: wholly gone, no blade.
    let cellAway = length(cellRelative.xz);
    let gone = bladeSpriteShare(cellAway);
    let count = u.grass.z * select(u.grass.w * clumpSize, bladeTuftCount, tuft);
    if (f32(number) >= count || gone >= 1.0) { return out; }

    // One texel of the tall grass's picture, its own: the next of a few
    // tries that isn't see-through (none: no blade).
    let layer = textureLayers[textureIndex] & layerMask;
    let pictureSize = vec2f(textureDimensions(blockTextures).xy);
    var texel = vec2f(0.0);
    var found = false;
    for (var k = 0u; k < 6u && !found; k++) {
        let at = (floor(fract(r2.xy + f32(k) * vec2f(0.7548776662, 0.5698402910)) * 16.0) + 0.5) / 16.0;
        if (textureLoad(blockTextures, vec2i(at * pictureSize), i32(layer), 0).a > 0.5) {
            texel = at;
            found = true;
        }
    }
    if (!found) { return out; }

    let height = select(mix(0.16, 0.42, r.w) * clumpSize, mix(0.4, 0.85, r.w), tuft) * mix(0.85, 1.0, lush);
    // Most of a sprite's texel wide (a 16th of a block).
    let width = select(0.042, 0.055, tuft) * mix(0.8, 1.2, r2.x);

    // Its turn: its own, pulled part of the way to facing the camera so
    // fewer are seen edge on.
    let angle = r2.y * 6.2831853;
    var right = vec3f(cos(angle), 0.0, sin(angle));
    let toCamera = normalize(vec2f(-base.x, -base.z) + vec2f(0.0001, 0.0));
    let across = vec3f(-toCamera.y, 0.0, toCamera.x);
    if (dot(right, across) < 0.0) { right = -right; }
    if (!forShadow) { right = normalize(mix(right, across, 0.4)); }

    // Its own lean, and the wind: gusts of noise travelling downwind, and a
    // flutter of its own.
    let t = u.animation.x;
    let leanAngle = r2.z * 6.2831853;
    let lean = vec3f(cos(leanAngle), 0.0, sin(leanAngle)) * mix(0.08, 0.6, r2.w * r2.w);
    let windDirection = normalize(vec2f(1.0, 0.35));
    let gust = 0.7 * bladeNoise(world * 0.09 - windDirection * t * 0.55)
        + 0.3 * bladeNoise(world * 0.31 - windDirection * t * 1.1 + vec2f(5.2, 9.1));
    let sway = (0.1 + 1.5 * gust * gust) * u.grass.y * 0.6 + sin(t * (2.2 + r.x * 1.5) + r.y * 6.2831853) * 0.03;
    let bend = lean + vec3f(windDirection.x, 0.0, windDirection.y) * sway;

    // The strip: a pair of corners per level, then the tip. It keeps its
    // width low down and narrows toward the tip, as a blade does.
    let level = min(corner / 2u, segments);
    let along = f32(level) / f32(segments);
    var side = select(-1.0, 1.0, (corner & 1u) == 1u);
    if (level == segments) { side = 0.0; }
    let taper = 1.0 - along * along;
    // Bent along its length, a little shorter the more it bends.
    let rise = height * along * (1.0 - 0.3 * min(dot(bend, bend), 1.0));
    let relative = base + vec3f(0.0, rise, 0.0) + bend * height * along * along
        + right * side * width * 0.5 * taper;

    out.position = psxSnap(u.viewProjection * vec4f(relative, 1.0));
    out.uv = texel;
    out.affineUV = texel;
    out.layer = layer;
    out.normalLayer = textureLayers[textureIndex + 1u] & layerMask;
    out.materialLayer = textureLayers[textureIndex + 2u] & layerMask;
    // Lit as the sprite is: its faces' fixed darkening, and the normal of
    // one of its two crossed planes, the side toward the camera (the side of
    // a sprite that is seen). That normal is for the light in the player's
    // hand; the sun takes both as the ground (sunFacing).
    out.ambientMultiplier = vertexColor[6];
    out.relative = relative;
    let plane = select(vec3f(0.70710678, 0.0, 0.70710678), vec3f(0.70710678, 0.0, -0.70710678), r.z < 0.5);
    out.normal = plane * select(-1.0, 1.0, dot(plane, base) <= 0.0);
    out.flags = 0u;
    out.blockPos = cell.xyz;
    out.inBlock = relative - cellRelative;
    out.light = light;
    out.colors = 0u;
    // Its tint, a little lighter or darker than the next; how much of it
    // has gone to its sprite; and which blade it is, for bladeDissolved (in
    // steps, so the fragment stage reads the same number all over it).
    out.blade = vec4f(1.0 + along, mix(0.93, 1.07, r2.w), gone, (floor(r.y * 1024.0) + 0.5) / 1024.0);
    return out;
}

// The blades' depth prepass: only the depth, less the pieces of a blade that
// have gone to its sprite.
@fragment
fn fs_bladeDepth(in: VertexOutput) {
    if (bladeDissolved(in)) { discard; }
}

// ---- Shells ---------------------------------------------------------------
// Roadmap S0: Acerola's shell-texturing demo (Shell.shader, SimpleShell.cs),
// on the grass tops near the camera instead of on a ball. For looking at
// the technique: off unless the panel's "Shells" or OURCRAFT_SHELLS turns
// it on.
//
// The idea: a surface is drawn again and again, each copy (a shell, a
// layer) a little farther out along its normal. Every layer is cut full of
// holes, and the holes grow from one layer to the next, so what is left
// standing through the stack is strands: a strand is the same small disc on
// layer after layer, shrinking to nothing at its own height.
//
// What is the demo's, under the demo's names (u.shellShape and the rest):
// where a layer lies (vs_shell: the length and its distance attenuation),
// the push that leaves the roots where they are (the curvature and the
// displacement strength), the strands (shellCut: the density, the noise's
// range, the thickness) and the dark roots (shellOcclusion: the attenuation
// and the bias).
//
// What is different, and why:
//  - One draw. The demo makes an object per layer, each with its own draw
//    and its own copy of every uniform. Here an instance is a cell (the
//    blades' own list, grassCells) and the vertex index says which layer
//    and which corner, as it says which blade in vs_blade. The uniforms are
//    set once.
//  - Layer 0 is not drawn: it is the block's own top, which is there
//    already. The top takes layer 0's colour instead (shellGround, shade).
//    The demo's layer 0 is the ball itself, never cut, for the same reason:
//    no holes to see through.
//  - A strand's number is its place in the world in whole numbers, so the
//    strands of two blocks never repeat. The demo's `x + 100 * y` repeats
//    along diagonals once the density passes 100.
//  - The light. The demo's is half Lambert, squared: on a top, where every
//    layer faces up, that is one number for the whole lawn. So the layers
//    are lit as the top is (shade), and only the roots' darkening is kept.
//  - The cost is the pixels: a lawn that fills the screen is shaded once
//    per layer. With the depth prepass the layers' depth goes down first,
//    with a fragment stage that only cuts the holes (fs_shellDepth), and
//    the light is worked out once a pixel, where the depth is Equal
//    (fs_shell). The layers are drawn top first, so that what is under a
//    strand can fail the depth test.

// The demo's hash, Hugo Elias's: a whole number shuffled to 0..1. (Its last
// constant is written 0x1376312589 there, more than 32 bits hold; this is
// what is left of it.)
fn shellHash(seed: u32) -> f32 {
    var n = seed;
    n = (n << 13u) ^ n;
    n = n * (n * n * 15731u + 0x789221u) + 0x76312589u;
    return f32(n & 0x7fffffffu) / 2147483647.0;
}

// How much light reaches a strand at height `h` (0 the root, 1 the tip of
// the tallest): the demo's stand-in for the shade of the strands around
// it, an exponent that keeps the dark low down and a bias so it isn't black.
fn shellOcclusion(h: f32) -> f32 {
    var occlusion = u.shellShade.y;
    if (h > 0.0) { occlusion += pow(h, u.shellShade.x); }
    return clamp(occlusion, 0.0, 1.0);
}

// Whether this pixel of a layer is one of its holes.
fn shellCut(in: VertexOutput) -> bool {
    // The top's 0..1 stretched over `density` cells each way: a strand a
    // cell. The whole part says which cell, the rest where in it, moved to
    // -1..1 so the cell's middle is 0.
    let density = u.shellStrands.x;
    let newUV = in.shell.zw * density;
    let localUV = fract(newUV) * 2.0 - 1.0;
    let fromCentre = length(localUV);

    // The strand's number, and from it its height: somewhere in the
    // noise's range.
    let tid = in.blockPos.xz * i32(density) + vec2i(floor(newUV));
    let seed = bitcast<u32>(tid.x) * 0x8da6b343u ^ bitcast<u32>(tid.y) * 0xd8163841u;
    let rand = mix(u.shellStrands.z, u.shellStrands.w, shellHash(seed));

    // A strand is a cone: as wide as the thickness at its root, a point at
    // its own height, nothing above. Past the cone's edge is a hole.
    let h = in.shell.x;
    return fromCentre > u.shellStrands.y * (rand - h);
}

// A top the shells stand on (the blades' ground, in the shells' reach): it
// is layer 0, and is coloured as one.
fn shellGround(in: VertexOutput) -> bool {
    if ((in.flags & bladeGroundFlag) == 0u || u.shellShape.x < 2.0) { return false; }
    let cell = in.blockPos + vec3i(0, 1, 0);
    let cellRelative = vec3f(cell - u.positionInt.xyz) - u.positionFloat.xyz;
    return length(cellRelative.xz) < u.shellShape.w;
}

// The layer view (u.shellColour.w): a layer's own colour, four taking
// turns, so two layers next to each other are told apart; the top is black.
// A white ring lies every 4 blocks from the camera, to read distances off.
fn shellLayerColour(layer: u32, relative: vec3f) -> vec3f {
    const colours = array<vec3f, 4>(
        vec3f(0.9, 0.12, 0.1), vec3f(0.9, 0.75, 0.08), vec3f(0.1, 0.7, 0.2), vec3f(0.12, 0.35, 0.95));
    let away = length(relative.xz);
    if (away > 2.0 && abs(fract(away / 4.0 + 0.5) - 0.5) * 4.0 < 0.03) { return vec3f(0.8); }
    if (layer == 0u) { return vec3f(0.0); }
    return colours[layer % 4u] * 0.3;
}

@vertex
fn vs_shell(@builtin(vertex_index) vertexIndex: u32, @builtin(instance_index) instance: u32) -> VertexOutput {
    var out: VertexOutput;
    out.position = bladeHidden;

    let layers = u32(u.shellShape.x);
    if (layers < 2u) { return out; }
    let cell = grassCells[instance];
    let cellRelative = vec3f(cell.xyz - u.positionInt.xyz) - u.positionFloat.xyz;
    if (length(cellRelative.xz) >= u.shellShape.w) { return out; }

    // Six vertices a layer: the four corners of the top as a strip, the
    // first and the last doubled to join it to the next layer with
    // triangles of no area. The top layer first.
    let layer = layers - 1u - vertexIndex / 6u;
    let corner = u32(clamp(i32(vertexIndex % 6u) - 1, 0, 3));
    let at = vec2f(f32(corner & 1u), f32(corner >> 1u));

    // The layer's height among the layers, 0..1, and where that puts it:
    // the exponent crowds the layers toward the top (under 1) or the tips
    // (over 1).
    let h = f32(layer) / f32(layers);
    let height = pow(h, u.shellShape.z);
    var relative = cellRelative + vec3f(at.x - 0.5, -0.5 + u.shellShape.y * height, at.y - 0.5);

    // The push: all of it at the tips, less lower down by the curvature,
    // none at the root. The demo's direction follows the ball about and
    // falls at rest; this one is fixed, downwind.
    let k = pow(height, u.shellPush.w);
    relative += u.shellPush.xyz * k * u.shellShade.z;

    out.position = psxSnap(u.viewProjection * vec4f(relative, 1.0));
    out.uv = at;
    out.affineUV = at;
    out.layer = 0u;
    out.normalLayer = 0u;
    out.materialLayer = 0u;
    out.ambientMultiplier = vertexColor[2];
    out.relative = relative;
    out.normal = vec3f(0.0, 1.0, 0.0);
    out.flags = 0u;
    out.blockPos = cell.xyz;
    out.inBlock = relative - cellRelative;
    out.light = u32(cell.w) & 0xFFu;
    out.colors = 0u;
    out.blade = vec4f(0.0);
    out.shell = vec4f(h, f32(layer), at);
    return out;
}

// The layers' depth prepass: only the holes.
@fragment
fn fs_shellDepth(in: VertexOutput) {
    if (shellCut(in)) { discard; }
}

// A layer lit: the shell's colour, darker toward the root, in the light the
// top would have there. No picture is read: flat, rough, not metal.
@fragment
fn fs_shell(in: VertexOutput) -> GBuffer {
    var t: Texels;
    t.albedo = vec4f(fromLinear(u.shellColour.rgb * shellOcclusion(in.shell.x)), 1.0);
    t.normalMap = vec3f(0.5, 0.5, 1.0);
    t.material = vec3f(0.05, 0.0, 0.0);
    t.ao = 1.0;
    t.water.p = vec2f(0.0);
    t.water.dx = vec2f(0.0);
    t.water.dy = vec2f(0.0);
    t.footprint = max(length(dpdx(in.relative)), length(dpdy(in.relative)));
    // After the prepass Equal has dropped the holes already; this is for
    // the prepass off.
    if (shellCut(in)) { discard; }

    let s = shade(in, t, true);
    var lit = s.color;
    if (!lobeView()) { lit = reflectSurface(in, s).color; }
    var out: GBuffer;
    out.color = vec4f(applyFog(in, lit), 1.0);
    if (u.shellColour.w != 0.0) { out.color = vec4f(shellLayerColour(u32(round(in.shell.y)), in.relative), 1.0); }
    out.position = vec4f((u.view * vec4f(in.relative, 1.0)).xyz, s.roughness);
    out.normal = packNormal(s.normal);
    out.bloom = vec4f(s.bloom, 0.0);
    return out;
}

// Milestone 13: the sun's shadow maps (renderShadow, zpass.vert with the
// cascade's matrix). The same corner, placed by a cascade's matrix instead
// of the camera's; fs_depth's alpha test lets leaves and grass cast their
// cut-out shapes. Each cascade is its own pipeline, picked by this override.
override shadowCascade: u32 = 0u;

@vertex
fn vs_shadow(@builtin(vertex_index) vertexIndex: u32, face: FaceInput) -> VertexOutput {
    var out = faceVertex(vertexIndex, face, true);
    out.position = u.shadowMatrices[shadowCascade] * vec4f(out.relative, 1.0);
    return out;
}

// The depth prepass (zpass.frag): no color, only the alpha test.
@fragment
fn fs_depth(in: VertexOutput) {
    let albedo = textureSample(blockTextures, blockSampler, faceUV(in), in.layer);
    if (albedo.a <= 0.0 || spriteDissolved(in)) { discard; }
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
    footprint: f32,    // how much of the world the pixel covers, in blocks (waterWaves)
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
    t.footprint = max(length(dpdx(in.relative)), length(dpdy(in.relative)));
    return t;
}

// ---- The rain on the ground -----------------------------------------------
// While it rains (u.wet.x rises, and falls again after), what the open sky
// reaches gets wet: darker and shinier. Past a point puddles fill on the
// tops of blocks, in the low places of a height made of noise, so they grow
// from their middles as it goes on raining and shrink back as it dries. A
// puddle is flat and smooth: it mirrors the sky and what stands around it
// (reflectPuddle), more the shallower the view. While rain falls, rings
// spread on it.
//
// All of it per texel of the world's grid, 16 to a block, as the held light
// is: a puddle's edge runs along texels and its rings are made of them, like
// the pictures on the blocks. In whole numbers, so it is the same however
// far the world is from the origin.

// Three numbers 0..1 for a point of a whole-number grid and a `salt`.
fn groundHash(p: vec2i, salt: i32) -> vec3f {
    var h = bitcast<u32>(p.x) * 0x8da6b343u ^ bitcast<u32>(p.y) * 0xd8163841u ^ bitcast<u32>(salt) * 0xcb1ab31fu;
    h = (h ^ (h >> 15u)) * 0x2c1b3c6du;
    h = (h ^ (h >> 12u)) * 0x297a2d39u;
    h = h ^ (h >> 15u);
    let h2 = (h ^ (h >> 16u)) * 0x7feb352du;
    let h3 = (h2 ^ (h2 >> 15u)) * 0x846ca68bu;
    return vec3f(f32(h >> 8u), f32(h2 >> 8u), f32(h3 >> 8u)) / 16777216.0;
}

// Which cell of `size` texels a texel is in (rounding down, below zero too).
fn groundCell(texel: vec2i, size: i32) -> vec2i {
    return (texel - vec2i(wrapCell(texel.x, size), wrapCell(texel.y, size))) / size;
}

// Smooth noise 0..1 over the world's texels, `period` texels from one of its
// lattice points to the next.
fn groundNoise(texel: vec2i, period: i32, salt: i32) -> f32 {
    let cell = groundCell(texel, period);
    let f = (vec2f(texel - cell * period) + 0.5) / f32(period);
    let s = f * f * (3.0 - 2.0 * f);
    let a = groundHash(cell, salt).x;
    let b = groundHash(cell + vec2i(1, 0), salt).x;
    let c = groundHash(cell + vec2i(0, 1), salt).x;
    let d = groundHash(cell + vec2i(1, 1), salt).x;
    return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}

// Puddles start to fill when the ground is this wet.
const puddlesFrom = 0.3;
// The rings: one drop at a time lands somewhere in each cell of this many
// texels, and its ring spreads this many texels before it is gone.
const ringCell = 12;
const ringReach = 7.0;

struct Rings {
    slope: vec2f,  // the rings' tilt of the surface, along x and z
    crest: f32,    // a ring's crest (above 0) or the trough beside it (below), -1..1
};

// The rings the rain makes at a point `local` texels into its cell of
// ringCell texels, from that cell's drop and its eight neighbours'. Each
// cell has its own pace; each drop of a cell lands somewhere else in it,
// and the lighter the rain (u.wet.y), the more of them are skipped.
fn rainRings(cell: vec2i, local: vec2f) -> Rings {
    var r: Rings;
    r.slope = vec2f(0.0);
    r.crest = 0.0;
    let rain = u.wet.y;
    for (var j = -1; j <= 1; j++) {
        for (var i = -1; i <= 1; i++) {
            let c = cell + vec2i(i, j);
            let pace = groundHash(c, 3);
            let clock = u.animation.x / (0.8 + 0.9 * pace.x) + pace.y * 7.0;
            let drop = groundHash(c, 4 + i32(floor(clock)));
            if (drop.z > rain * 0.85) { continue; }
            let t = fract(clock);
            let away = local - (vec2f(f32(i), f32(j)) + drop.xy) * f32(ringCell);
            let distance = length(away);
            // A crest with a trough either side of it, flattening out as
            // it spreads.
            let across = (distance - t * ringReach) / 1.3;
            let ring = exp(-across * across * 0.5) * (1.0 - t);
            r.crest += cos(across * 2.2) * ring;
            r.slope += away / max(distance, 0.001) * sin(across * 2.2) * ring;
        }
    }
    return r;
}

struct WetGround {
    amount: f32,   // how wet the face is, 0..1
    puddle: f32,   // how much of a puddle this texel is, 0..1
    slope: vec2f,  // the rings' tilt of the puddle's surface (rainRings)
    crest: f32,    // a ring's crest or trough (rainRings)
};

fn groundWet(in: VertexOutput, levels: LightLevels, water: bool) -> WetGround {
    var g: WetGround;
    g.amount = 0.0;
    g.puddle = 0.0;
    g.slope = vec2f(0.0);
    g.crest = 0.0;
    // Not under the snow weather's cover, not water or what is under it,
    // and not what faces down.
    let wetness = u.wet.x * (1.0 - u.wet.z);
    if (wetness <= 0.0 || water || (in.flags & 2u) != 0u || in.normal.y < -0.5) { return g; }
    // Where the open sky reaches (the baked sky light, 15 under it and a
    // level less a block sideways), fading out under a roof's edge. The
    // tops of things take the rain whole, their sides some of it.
    let open = clamp(f32(levels.skyUnchanged - 12) / 3.0, 0.0, 1.0);
    g.amount = wetness * open * mix(0.6, 1.0, max(in.normal.y, 0.0));

    if ((in.flags & topOfCubeFlag) == 0u || wetness <= puddlesFrom || open < 1.0) { return g; }
    let texel = in.blockPos.xz * 16 + vec2i(clamp(floor((in.inBlock.xz + 0.5) * 16.0), vec2f(0.0), vec2f(15.0)));
    // The ground's height, for the water to find the low places of:
    // hollows two blocks or so across, with smaller ones in them.
    let height = 0.6 * groundNoise(texel, 34, 1) + 0.4 * groundNoise(texel, 14, 2);
    let level = mix(0.2, 0.37, smoothstep(puddlesFrom, 1.0, wetness));
    let depth = level - height;
    if (depth <= 0.0) { return g; }
    g.puddle = smoothstep(0.0, 0.025, depth);

    if (u.wet.y <= 0.0) { return g; }
    let cell = groundCell(texel, ringCell);
    let rings = rainRings(cell, vec2f(texel - cell * ringCell) + 0.5);
    g.slope = rings.slope;
    g.crest = rings.crest;
    return g;
}

// The normal a face takes the sun by: its own, but straight up for grass (a
// plant's crossed sprite, a blade), as the ground it stands on. The side of
// a sprite that is seen faces the camera, and a blade's normal is made to,
// so by their own normals the sun lit them on one side of a line through
// the camera and not on the other: looking along a sprite's diagonal
// (yaw 45, 135, 225, 315) the line ran up the middle of the screen, half
// the field in the sun and half out of it, and it moved with the player.
fn isGrass(in: VertexOutput) -> bool {
    return in.blade.x > 0.0 || (in.flags & plantSpriteFlag) != 0u;
}

fn sunFacing(in: VertexOutput) -> vec3f {
    if (isGrass(in)) { return vec3f(0.0, 1.0, 0.0); }
    return in.normal;
}

// Water's surface between its waves, and its reflectance looking straight
// at it (an index of refraction of 1.33).
const waterRoughness = 0.11;
const waterF0 = 0.02;

// Milestone 18: with the camera under water, a colour darkens toward the
// underwater colour with distance (GL's computeFogUnderWater).
fn underWaterFog(color: vec3f, distance: f32) -> vec3f {
    if (u.fogParams.z == 0.0) { return color; }
    let fog = pow(exp(-pow(distance / u.underWaterParams.x, u.underWaterParams.y)), 4.0);
    let strength = u.underWaterColor.w;
    return mix(u.underWaterColor.rgb, color, vec3f(fog * strength + (1.0 - strength)));
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
    puddle: f32,    // how much of a puddle it is, 0..1 (groundWet)
    crest: f32,     // a puddle's ring's crest (above 0) or trough (below)
    // For water (waterColour): the ambient light, how much of the sun
    // reaches it, the waves' height there and the surface's tilt from its
    // face, in the world.
    ambient: vec3f,
    shadow: f32,
    waveHeight: f32,
    tilt: vec3f,
    ripple: f32,    // how much foam the rings on it make (surfaceRipples)
};

// defaultShader.frag's lit path (u_shaders on), fog aside.
fn shade(in: VertexOutput, t: Texels, frontFacing: bool) -> Shaded {
    let levels = lightLevels(in.light);
    let water = isWater(in);
    let ao = aoFromTexel(in.flags, t.ao);

    // The sun's shadow, with the face's own normal (GL's v_normal) and the
    // distance ahead of the camera that picks the cascade.
    let viewDepth = -(u.view * vec4f(in.relative, 1.0)).z;
    // A grass blade asks from just above its block: its tuft's sprite is
    // still in the shadow maps, to shade the ground, and would shade the
    // blades standing inside it at random.
    var shadowFrom = in.relative;
    if (in.blade.x > 0.0) { shadowFrom.y += 0.55 - in.inBlock.y; }
    let sunNormal = sunFacing(in);
    let shadow = sunShadow(shadowFrom, viewDepth, dot(u.sunDirection.xyz, sunNormal), in.position.xy);

    let ambient = computeAmbient(withHeldLight(levels, in.relative, in.normal), sunNormal, in.ambientMultiplier,
        (in.flags & 2u) != 0u, ao, shadow);

    var m = material(t.material);
    var albedo = toLinear(t.albedo.rgb);
    if (water) {
        // Water mirrors and lets light through; it has no surface colour of
        // its own (waterColour adds what the water under it sends back), so
        // the lights below leave only their glints. Smooth, not metal.
        albedo = vec3f(0.0);
        m.roughness = waterRoughness;
        m.metallic = 0.0;
    }
    // (On water those bits say which way it runs, not a paint.)
    if (!water) { albedo = paintAlbedo(albedo, in.colors); }
    if (bladeGroundShaded(in)) { albedo *= bladeGroundShade; }
    // Under the shells the top is their layer 0: their colour, at the root.
    if (in.shell.x == 0.0 && shellGround(in)) { albedo = u.shellColour.rgb * shellOcclusion(0.0); }

    // The snow weather's cover: the tops of blocks under the open sky (the
    // sky light above them unblocked) whiten as it snows, keeping some of
    // the texture's shading, and go rough and dull.
    if (u.wet.z > 0.0 && !water && in.normal.y > 0.7 && levels.skyUnchanged >= 15) {
        let cover = u.wet.z * 0.9;
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

    // The rain on the ground: wet, it is darker and shinier (though not
    // smooth enough for reflectSurface's reflections); a puddle is darker
    // still, and as smooth as water.
    let wet = groundWet(in, levels, water);
    if (wet.amount > 0.0) {
        albedo *= mix(1.0, 0.62, wet.amount);
        m.roughness = mix(m.roughness, max(m.roughness * 0.5, 0.46), wet.amount);
        albedo *= mix(1.0, 0.7, wet.puddle);
        m.roughness = mix(m.roughness, 0.07, wet.puddle);
        m.metallic = mix(m.metallic, 0.0, wet.puddle);
    }

    // Water's normal is GL's moving normal.png (18-parity), the rest the
    // block's own normal map. A puddle lies flat over the block's bumps,
    // tilted only by its rings.
    var normalTexel = t.normalMap;
    if (water) { normalTexel = waterNormalTexel(t.water); }
    var N = applyNormalMap(in.normal, normalTexel, frontFacing);
    // Water's surface (a face that looks up) tilts by its waves instead;
    // the ones too small for this pixel roughen it.
    var waveHeight = 0.0;
    var ringCrest = 0.0;
    var rippleFoam = 0.0;
    if (water && in.normal.y > 0.5) {
        let waves = waterWaves(vec2f(in.blockPos.xz & vec2i(1023)) + in.inBlock.xz, waveCount, t.footprint);
        waveHeight = waves.height;
        var slope = waves.slope;
        // Running water: its own waves, travelling the way it runs.
        if (in.colors >= 1u && in.colors <= 8u) {
            slope = slope * 0.5 + waterFlowWaves(vec2f(in.blockPos.xz & vec2i(1023)) + in.inBlock.xz,
                in.colors, t.footprint);
        }
        // The rain's rings, as on the puddles but smooth (the puddles' are
        // made of texels): under the open sky, and near enough to show (a
        // ring is about a quarter of a block from crest to crest).
        let ringsShown = 1.0 - smoothstep(0.25, 0.5, t.footprint / 0.23);
        if (u.wet.y > 0.0 && levels.skyUnchanged >= 15 && ringsShown > 0.0) {
            let within = (in.inBlock.xz + 0.5) * 16.0;
            let texel = in.blockPos.xz * 16 + vec2i(floor(within));
            let cell = groundCell(texel, ringCell);
            let rings = rainRings(cell, vec2f(texel - cell * ringCell) + fract(within));
            slope -= rings.slope * 0.8 * ringsShown;
            ringCrest = rings.crest * ringsShown;
        }
        // The rings round what meets the surface.
        if (u.animation.w > 0.0) {
            let ripples = surfaceRipples(in, t.footprint);
            slope += ripples.slope;
            rippleFoam = ripples.foam;
        }
        N = normalize(in.normal - vec3f(slope.x, 0.0, slope.y) * in.normal.y);
        if (!frontFacing) { N = -N; }
        // GGX's alpha is the roughness squared, and twice the slopes'
        // variance is alpha squared.
        let alpha = waterRoughness * waterRoughness;
        m.roughness = clamp(sqrt(sqrt(alpha * alpha + 2.0 * waves.lost)), waterRoughness, 0.6);
    }
    // A wave's far side can face away from the eye; the eye sees the near
    // side of the next one instead. Turned just into view.
    if (water) {
        let facing = dot(N, -normalize(in.relative));
        if (facing < 0.03) { N = normalize(N - normalize(in.relative) * (0.03 - facing)); }
    }
    if (wet.puddle > 0.0) {
        let surface = normalize(in.normal + vec3f(wet.slope.x, 0.0, wet.slope.y) * 0.35);
        N = normalize(mix(N, surface, wet.puddle));
    }

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
    if (water) { F0 = vec3f(waterF0); }
    F0 = mix(F0, vec3f(0.02), wet.puddle);

    let blockLights = pointLights(levels, in.blockPos, in.inBlock, m.metallic, m.roughness, V, albedo, N, F0, caustics);
    var sunN = N;
    if (isGrass(in)) { sunN = sunNormal; }
    let sunLobes = sunLight(levels, m.metallic, m.roughness, V, albedo, sunN, F0, ao, shadow, caustics);
    let sun = lobesTotal(sunLobes);
    let direct = lobesSum(blockLights, sunLobes);
    let color = lobesTotal(direct);

    var s: Shaded;
    s.dotNV = dot(N, V);
    // Milestone 18: under water, the color darkens toward the underwater
    // color with distance (GL's computeFogUnderWater), before the
    // reflections and the water's own mix, as in GL.
    var lit = color + albedo * ambient;
    // (Water's is done on its whole colour, in waterColour.)
    if (!water) { lit = underWaterFog(lit, length(in.relative)); }
    // One part of the light alone (roadmap L0), as it left the surface.
    if (lobeView()) { lit = lightShown(direct, albedo * ambient); }
    s.alpha = 1.0 - (1.0 - t.albedo.a) * s.dotNV;
    // GL's ambient without its physically accurate reflections (switched
    // off by a constant): the albedo in the ambient light.
    s.color = lit;
    s.normal = N;
    s.roughness = m.roughness;
    s.bloom = m.emissive * 0.12 * albedo;
    // The brightest caustics glow (GL adds this to out_bloom with the sun).
    if (causticsPower > 1.2) { s.bloom += max(sun, vec3f(4.0)) * 0.0008; }
    if (lobeView()) { s.bloom = vec3f(0.0); }
    s.F0 = F0;
    s.puddle = wet.puddle;
    s.crest = wet.crest + ringCrest;
    s.ambient = ambient;
    s.shadow = shadow;
    s.waveHeight = waveHeight;
    s.tilt = N - select(-in.normal, in.normal, frontFacing);
    s.ripple = rippleFoam;
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
    // 1 where the ray landed well inside the screen, down to 0 at its edge
    // (for water, which fades to the sky there rather than cutting to it).
    amount: f32,
};

// GL's SSR(): the color last frame had where the reflected ray lands, or
// not found (off screen, toward the camera, faded out at the screen's edge).
fn ssr(viewPos: vec3f, N: vec3f, roughness: f32, wp: vec3f, viewDir: vec3f, viewSpaceNormal: vec3f) -> Reflection {
    var r: Reflection;
    r.color = vec3f(0.0);
    r.found = false;
    r.amount = 0.0;
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
    let inside = min(march.coord, vec2f(1.0) - march.coord);
    r.amount = smoothstep(0.0, 0.08, min(inside.x, inside.y));
    return r;
}

fn fresnelSchlickRoughness(cosTheta: f32, F0: vec3f, roughness: f32) -> vec3f {
    return F0 + (max(vec3f(1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// The sky's colour in a direction, from the sky as drawn behind this frame
// (skyColor): where on the screen that direction is, or the nearest edge
// when it is off it. For a mirror that finds nothing else to show.
fn skyToward(direction: vec3f) -> vec3f {
    let clip = u.viewProjection * vec4f(direction, 0.0);
    var uv = vec2f(0.5, 0.0); // behind the camera: the top of the screen
    if (clip.w > 0.0001) { uv = clamp(vec2f(clip.x, -clip.y) / clip.w * 0.5 + 0.5, vec2f(0.0), vec2f(1.0)); }
    let size = vec2f(textureDimensions(skyColor));
    return textureLoad(skyColor, clamp(vec2i(uv * size), vec2i(0), vec2i(size) - 1), 0).rgb;
}

// A puddle's mirror: what the reflected ray lands on (with the SSR setting
// on), or the sky that way. Little of it looking straight down, where the
// ground shows through, and most of it at a shallow angle; a ring's crest
// catches more of it and its trough less, which is what shows the rings
// under a sky that is the same grey all over.
fn reflectPuddle(in: VertexOutput, s: Shaded) -> Reflection {
    var r: Reflection;
    r.found = false;
    r.amount = 0.0;
    let V = -normalize(in.relative);
    var mirrored = skyToward(reflect(-V, s.normal));
    if (u.lightParams.w != 0) {
        let viewPos = (u.view * vec4f(in.relative, 1.0)).xyz;
        let viewSpaceNormal = normalize((u.view * vec4f(s.normal, 0.0)).xyz);
        let found = ssr(viewPos, in.normal, s.roughness * s.roughness, in.relative, V, viewSpaceNormal);
        if (found.found) {
            mirrored = found.color;
            r.found = true;
        }
    }
    let shallow = pow(1.0 - clamp(s.dotNV, 0.0, 1.0), 3.0);
    let strength = clamp(0.12 + 0.88 * shallow + clamp(s.crest, -1.0, 1.0) * 0.3, 0.0, 1.0) * s.puddle;
    r.color = mix(s.color, mirrored, strength);
    return r;
}

// defaultShader.frag's "ssr" block, with the SSR setting on, on faces
// smoother than 0.45. GL reads the face's view position from last frame's
// positions at the pixel; this uses the face's own, which is the same
// while the camera is still and right while it moves. The reflection is
// mixed in by the face's fresnel and roughness. (Water has its own:
// waterColour.)
fn reflectSurface(in: VertexOutput, s: Shaded) -> Reflection {
    if (s.puddle > 0.0) { return reflectPuddle(in, s); }
    var r: Reflection;
    r.color = s.color;
    r.found = false;
    r.amount = 0.0;
    if (u.lightParams.w == 0 || s.roughness >= 0.45) { return r; }
    let viewPos = (u.view * vec4f(in.relative, 1.0)).xyz;
    let viewSpaceNormal = normalize((u.view * vec4f(s.normal, 0.0)).xyz);
    let V = -normalize(in.relative);
    let found = ssr(viewPos, s.normal, s.roughness * s.roughness, in.relative, V, viewSpaceNormal);
    if (!found.found) { return r; }
    r.found = true;
    r.amount = found.amount;
    let fresnel = fresnelSchlickRoughness(s.dotNV, s.F0, s.roughness);
    r.color = mix(mix(found.color, s.color, fresnel), s.color, pow(s.roughness, 0.5));
    return r;
}

// defaultShader.frag's computeFog (after Sildur's shaders, as GL notes):
// how much of the lit color is left at `dist` blocks from the camera, the
// rest going to the sky behind. A near haze thickening with distance, more
// so low down (airDensity grows with height above 60), and a hard edge at
// u_fogDistance so the world's end fades into the sky (here the horizon fog
// instead, horizonFogAmount). GL's rainStrength was a constant 0 there;
// milestone G1 feeds it from the weather (fogParams.w): rain thins the
// density a little, flattens the height falloff and raises the haze. GL's time-of-day term (tmult) was commented out, its rain end
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

    return (1.0 - horizonFogAmount(distance)) * near;
}

// The horizon fog: how much of the world has gone into the sky behind it at
// `distance` blocks, 0..1. A PlayStation's fog, by distance alone, from
// u.horizonFog.y to wholly at u.horizonFog.x, which the renderer keeps short
// of the nearest chunk not drawn yet: a chunk still to come is a hole that
// shows the sky, and the fog is the sky's colour, so it is not seen. GL's
// was a hard edge at u_fogDistance (exp(-(d/fogDistance)^32)), which hid
// nothing on the way.
fn horizonFogAmount(distance: f32) -> f32 {
    let end = u.horizonFog.x;
    let start = min(u.horizonFog.y, end - 1.0);
    let t = clamp((distance - start) / (end - start), 0.0, 1.0);
    // Gentle at first, most of it toward the end.
    return t * t * (3.0 - 2.0 * t);
}

// GL's last step: the color toward the sky behind it, above water; then
// the weather's fog.
fn applyFog(in: VertexOutput, color: vec3f) -> vec3f {
    if (u.fogParams.z != 0.0 || lobeView()) { return color; }
    let sky = textureLoad(skyColor, vec2i(in.position.xy), 0).rgb;
    let fogged = mix(sky, color, computeFog(length(in.relative), f32(in.blockPos.y) + in.inBlock.y));
    return applyWeatherFog(fogged, in.relative, in.normal, lightLevels(in.light).skyUnchanged);
}

// ---- Foam ---------------------------------------------------------------------
// Foam is drawn the way a PlayStation would: white or not at all, a cell at
// a time, on a grid of foamCells cells to a block that lies on the water.
// How much foam there is at a place (0..1) is how many of the cells there
// are white, by an ordered dither: thick foam is solid, thinner a
// checkerboard, thinner still a scatter of single cells. The grid is the
// world's, so the pattern stays on the water as the camera turns. Too far
// away to tell the cells apart, the foam is blended by its amount instead.
//
// Two things make foam: the shore (shoreFoam) and whatever meets the surface
// (surfaceRipples).

// Twice the block textures' texels: a shore's band is a third of a block
// wide, and needs the cells to fade out over.
const foamCells = 32;

// The patches the foam breaks up in, 0..1: two noises a tenth and a
// twentieth of a block across, drifting apart, on a grid 64 to a block
// (groundNoise takes whole numbers). Too far to tell them apart, an even
// half.
fn foamPatches(in: VertexOutput, footprint: f32) -> f32 {
    let fine = in.blockPos.xz * 64 + vec2i(floor((in.inBlock.xz + 0.5) * 64.0));
    let drift = i32(floor(u.animation.x * 7.0));
    let patches = 0.55 * groundNoise(fine + vec2i(drift, drift / 2), 7, 21)
        + 0.45 * groundNoise(fine - vec2i(drift / 2, drift), 3, 22);
    return mix(patches, 0.5, smoothstep(0.02, 0.08, footprint));
}

// The cell of the foam's grid a pixel of water's surface is in.
fn foamCell(in: VertexOutput) -> vec2i {
    return in.blockPos.xz * foamCells + vec2i(floor((in.inBlock.xz + 0.5) * f32(foamCells)));
}

// How much of the pixel is foam, 0..1, where there is `amount` of it and
// the pixel is in `cell` of the foam's grid.
fn foamShown(cell: vec2i, amount: f32, footprint: f32) -> f32 {
    let dithered = select(0.0, 1.0, amount > bayer4(cell));
    let far = smoothstep(0.7 / f32(foamCells), 1.6 / f32(foamCells), footprint);
    return mix(dithered, clamp(amount, 0.0, 1.0), far);
}

// Where water's surface meets a solid block the waves break into foam: a
// band along that side of the water's block, and round a block that only
// touches its corner. Chunk::bake says which of the eight blocks around are
// solid (flag bits 16-23: +z, -z, -x, +x, then the corners -x+z, +x+z, -x-z,
// +x-z). The band is whole against the land and thins from there out, in
// patches that drift; it reaches further while the swell is high there and
// draws back as it falls.

// How far from the land the foam reaches, in blocks, with the swell low and
// high.
const foamReach = vec2f(0.28, 0.6);

// How much foam the shore makes at a pixel of water's surface, 0..1.
fn shoreFoam(in: VertexOutput, waveHeight: f32, footprint: f32) -> f32 {
    let shore = (in.flags >> 16u) & 0xFFu;
    if (shore == 0u || in.normal.y <= 0.5) { return 0.0; }

    // How far the pixel is from the nearest land: from each solid side's
    // edge, and from each solid corner's point.
    let p = in.inBlock.xz;
    var away = 10.0;
    if ((shore & 1u) != 0u) { away = min(away, 0.5 - p.y); }
    if ((shore & 2u) != 0u) { away = min(away, 0.5 + p.y); }
    if ((shore & 4u) != 0u) { away = min(away, 0.5 + p.x); }
    if ((shore & 8u) != 0u) { away = min(away, 0.5 - p.x); }
    if ((shore & 16u) != 0u) { away = min(away, length(vec2f(0.5 + p.x, 0.5 - p.y))); }
    if ((shore & 32u) != 0u) { away = min(away, length(vec2f(0.5 - p.x, 0.5 - p.y))); }
    if ((shore & 64u) != 0u) { away = min(away, length(vec2f(0.5 + p.x, 0.5 + p.y))); }
    if ((shore & 128u) != 0u) { away = min(away, length(vec2f(0.5 - p.x, 0.5 + p.y))); }

    let swell = clamp(0.5 + waveHeight / (0.12 * waveStrength()), 0.0, 1.0);
    var reach = mix(foamReach.x, foamReach.y, swell);
    // Water one block wide, land on both sides: less, or it is all foam.
    if ((shore & 3u) == 3u || (shore & 12u) == 12u) { reach *= 0.45; }
    if (away >= reach) { return 0.0; }

    let inward = 1.0 - away / reach; // 1 at the land, 0 where the band ends
    return clamp(inward * 1.35 - 0.12 + (foamPatches(in, footprint) - 0.5) * 0.5, 0.0, 1.0);
}

// The rings where something meets the surface (u.ripples; waterRipples.cpp
// makes them: a splash going in or coming out, a wake's ring every half
// block behind what moves along the surface). Each is a low ridge of water
// spreading from its middle, slowing and widening as it goes, with a
// smaller one following inside it; a splash also churns its middle for a
// moment. The ridges carry foam, less as they age, and tilt the surface.
struct Ripples {
    foam: f32,    // how much foam, 0..1 and over
    slope: vec2f, // the surface's tilt, along x and z
};

fn surfaceRipples(in: VertexOutput, footprint: f32) -> Ripples {
    var r: Ripples;
    r.foam = 0.0;
    r.slope = vec2f(0.0);
    let count = i32(u.animation.w);
    for (var i = 0; i < count; i++) {
        let place = u.ripples[i * 2];
        let kind = u.ripples[i * 2 + 1];
        let offset = in.relative - place.xyz;
        // Only on the water it is on: not a pool a block up or down.
        if (abs(offset.y) > 0.7) { continue; }
        let age = place.w;
        let radius = kind.z + 1.5 * pow(age, 0.85);
        let width = 0.07 + 0.06 * age;
        let away = length(offset.xz);
        if (away > radius + width * 3.0) { continue; }
        let left = 1.0 - clamp(age / kind.y, 0.0, 1.0);
        let fade = left * left * kind.x;

        let across = (away - radius) / width;
        let ridge = exp(-across * across);
        let across2 = (away - radius * 0.55) / width;
        let ridge2 = 0.5 * exp(-across2 * across2);
        var foam = 1.3 * ridge + ridge2;
        if (kind.w > 0.5) {
            foam += 1.2 * clamp(1.0 - age / 0.7, 0.0, 1.0)
                * (1.0 - smoothstep(kind.z * 0.5, kind.z * 1.3 + 0.6 * age, away));
        }
        r.foam += foam * fade;
        // A ridge 0.03 of a block high at full strength: its height's
        // change outward.
        r.slope += offset.xz / max(away, 0.001) * (-2.0 * (across * ridge + across2 * ridge2)) * (0.03 * fade / width);
    }
    // Ragged, as the shore's foam is; and none where there would only be a
    // cell here and there (so thin, the dither's cells stand in rows).
    if (r.foam > 0.0) { r.foam = max(r.foam * (0.55 + 0.9 * foamPatches(in, footprint)) - 0.16, 0.0) * 1.3; }
    return r;
}

// ---- Water's colour ---------------------------------------------------------
// What a pixel of water sends to the eye, after the "PBR" water of Garrett
// Gunnell's Water repository (its FFTWater.shader) and the references it
// lists:
//
//   the mirror   The share Fresnel gives (2% looking straight down, nearly
//                all of it along the surface; less on rough water, whose
//                slopes face every way): what the reflected ray lands on
//                (the SSR setting) or else the sky that way.
//   through it   The rest: what is under the water (the peel), shifted as
//                the waves bend the light, and dimmed by the water it
//                crosses, red first, then green, then blue (Beer-Lambert:
//                exp(-absorption * distance)). What it takes away it gives
//                back as its own colour, in the light there is (the waterColor
//                setting: the colour of deep water by day).
//   the sun      Its glints (shade's lights, on the waves' normal), and its
//                light coming through the crests of waves toward the eye.
//   the foam     At the shore and round what meets the surface (shoreFoam,
//                surfaceRipples): white in the light there is, dithered
//                (foamShown), over all of the above.
//
// From under water the surface is a window straight up (Snell's window) and
// a mirror of the deep beyond 49 degrees from it, where no light from above
// can get in (total internal reflection).

// How much each block of water takes of red, green and blue.
const waterAbsorption = vec3f(0.26, 0.075, 0.06);

// Fresnel's share for a surface as rough as `alpha` (GGX's), seen at
// `cosine` from its normal (the fit Gunnell's shader uses: rough water
// mirrors less at grazing angles than Schlick's smooth surface).
fn waterFresnel(cosine: f32, alpha: f32) -> f32 {
    let grazing = pow(max(1.0 - cosine, 0.0), 5.0 * exp(-2.69 * alpha)) / (1.0 + 22.7 * pow(alpha, 1.5));
    return clamp(waterF0 + (1.0 - waterF0) * grazing, 0.0, 1.0);
}

fn waterColour(in: VertexOutput, s: Shaded, t: Texels, frontFacing: bool) -> vec4f {
    let pixel = vec2i(in.position.xy);
    let V = -normalize(in.relative);
    let distance = length(in.relative);
    let N = s.normal;
    let cosine = clamp(s.dotNV, 0.0, 1.0);
    let alpha = s.roughness * s.roughness;
    let levels = lightLevels(in.light);
    // How much of the sky is over this water, by the baked sky light: none
    // in a cave.
    let open = clamp(f32(levels.skyUnchanged - 3) / 9.0, 0.0, 1.0);
    // Seen from inside the water (the back of a face).
    let inside = !frontFacing;

    // The mirror's share.
    var fresnel = waterFresnel(cosine, alpha);
    if (inside) {
        // Out of water into air the light bends away from the normal, and
        // past the critical angle none gets through.
        let sine2 = 1.33 * 1.33 * (1.0 - cosine * cosine);
        fresnel = 1.0;
        if (sine2 < 1.0) { fresnel = mix(waterFresnel(sqrt(1.0 - sine2), alpha), 1.0, smoothstep(0.9, 1.0, sine2)); }
    } else {
        // A rain ring's crest catches more of the sky and its trough less,
        // as on the puddles: what shows the rings under an even grey sky.
        fresnel = clamp(fresnel + clamp(s.crest, -1.0, 1.0) * 0.25, 0.0, 1.0);
    }

    // What it mirrors: the sky that way (a ray thrown downward by a wave's
    // far side would meet the next wave and go up after all), dark where no
    // sky is; from inside, the deep. Then what the ray lands on.
    var toward = reflect(-V, N);
    toward.y = abs(toward.y);
    var mirrored = mix(s.ambient * 0.25, skyColourToward(toward), open);
    if (inside) { mirrored = u.underWaterColor.rgb * mix(0.3, 1.0, open); }
    if (u.lightParams.w != 0) {
        let viewPos = (u.view * vec4f(in.relative, 1.0)).xyz;
        let viewSpaceNormal = normalize((u.view * vec4f(N, 0.0)).xyz);
        let found = ssr(viewPos, select(-in.normal, in.normal, frontFacing), waterRoughness * waterRoughness,
            in.relative, V, viewSpaceNormal);
        if (found.found) { mirrored = mix(mirrored, found.color, found.amount); }
    }

    // How much water the eye's ray crosses under this pixel: the peel's
    // depth less the surface's, along the ray.
    let surfaceDepth = linearizeDepth(in.position.z);
    let alongRay = distance / max(surfaceDepth, 0.001);
    var thickness = max(linearizeDepth(textureLoad(peelDepth, pixel, 0)) - surfaceDepth, 0.0) * alongRay;

    // The water's own light: its colour in the ambient light and some of the
    // sun's; and the sun through the waves, brightest in the crests when
    // looking toward it.
    let L = u.sunDirection.xyz;
    var sun = u.sunLightColor.rgb * s.shadow * open * mix(0.6, 1.0, lowSunStrength());
    if (L.y <= -0.2) { sun = vec3f(0.0); }
    let body = u.waterColor.rgb * (s.ambient + sun * 0.3 * max(L.y, 0.0));
    let crest = clamp(s.waveHeight / (0.06 * waveStrength()), 0.0, 1.0);
    let through = 1.4 * crest * pow(clamp(dot(L, -V), 0.0, 1.0), 4.0) * pow(0.5 - 0.5 * dot(L, N), 3.0)
        + 0.12 * cosine * cosine * max(L.y, 0.0);
    let glow = u.waterColor.rgb * vec3f(1.0, 1.5, 1.2) * sun * through;

    var color: vec3f;
    var opacity = 1.0;
    if (passMode == 1u) {
        // What is under the water, where the waves' tilt bends the ray to:
        // a fifth of the tilt (a quarter, 1 - 1/1.33, would be the whole of
        // it) over the water it crosses, as seen from this far. Where that lands on something in front of
        // the water, the straight lookup is kept.
        let size = vec2f(textureDimensions(peelColor));
        let bend = (u.view * vec4f(s.tilt, 0.0)).xy * 0.2 * min(thickness, 6.0) / max(distance, 0.5);
        let shift = bend * vec2f(u.projection[0][0], -u.projection[1][1]) * 0.5;
        let bentUv = in.position.xy / size + shift;
        let bentPixel = clamp(vec2i(bentUv * size), vec2i(0), vec2i(size) - 1);
        let bentDepth = linearizeDepth(textureLoad(peelDepth, bentPixel, 0));
        var under = textureLoad(peelColor, pixel, 0).rgb;
        if (bentDepth >= surfaceDepth + 0.1) {
            under = textureSampleLevel(peelColor, lastFrameSampler, bentUv, 0.0).rgb;
            thickness = (bentDepth - surfaceDepth) * alongRay;
        }
        if (inside) {
            // Looking out: air beyond the surface, nothing for it to dim.
            color = mix(under, mirrored, fresnel);
        } else {
            let kept = exp(-waterAbsorption * thickness);
            color = mix(under * kept + body * (vec3f(1.0) - kept) + glow, mirrored, fresnel);
        }
    } else {
        // No peel: the water's own colour and the mirror, blended over what
        // is behind by how much of it the water hides.
        let kept = exp(-waterAbsorption * thickness);
        let hidden = 1.0 - dot(kept, vec3f(1.0 / 3.0));
        color = mix(body + glow, mirrored, fresnel);
        opacity = clamp(fresnel + (1.0 - fresnel) * hidden, 0.0, 1.0);
        if (inside) { opacity = max(fresnel, 0.15); color = mirrored; }
    }
    color += s.color;
    // The foam, from above: matt and white, so it takes the ambient light
    // and the sun as the ground does, and hides the mirror and the glints.
    if (!inside) {
        var amount = max(shoreFoam(in, s.waveHeight, t.footprint), s.ripple);
        var cell = foamCell(in);
        // Running water: streaks the way it runs (and no shore, Chunk::bake).
        if (in.colors >= 1u && in.colors <= 8u && in.normal.y > 0.5) {
            amount = max(amount, flowFoam(in, t.footprint));
        }
        // A fall's streaks, on water's sides.
        if (in.colors == waterFalling && abs(in.normal.y) < 0.5) {
            let fall = fallFoam(in, t.footprint);
            amount = fall.amount;
            cell = fall.cell;
        }
        if (amount > 0.0) {
            let foam = foamShown(cell, amount, t.footprint);
            let lit = vec3f(0.86, 0.9, 0.94) * (s.ambient * 1.15 + sun * 0.6 * max(L.y, 0.0));
            color = mix(color, lit, foam);
            opacity = max(opacity, foam);
        }
    }
    return vec4f(applyFog(in, underWaterFog(color, distance)), opacity);
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
    var t = readTexels(in);
    // GL's alpha test: cut-out leaves, grass and flowers. After the prepass
    // this only matters with the prepass off; with it, Equal has already
    // dropped the cut-out pixels' fragments.
    if (t.albedo.a <= 0.0 || spriteDissolved(in) || bladeDissolved(in)) { discard; }
    if (in.blade.x > 0.0) { t.albedo = bladeAlbedo(in, t.albedo); }

    // A blade's normal already faces the camera, whichever way its strip
    // winds.
    let s = shade(in, t, frontFacing || in.blade.x > 0.0);
    var lit = s.color;
    if (!lobeView()) { lit = reflectSurface(in, s).color; }
    if (in.blade.x > 0.0) { lit *= in.blade.y; }
    var out: GBuffer;
    out.color = vec4f(applyFog(in, lit), 1.0);
    if (u.shellColour.w != 0.0 && shellGround(in)) { out.color = vec4f(shellLayerColour(0u, in.relative), 1.0); }
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

    // One part of the light alone (roadmap L0): glass and ice by their
    // alpha as ever, water opaque (it has no colour of its own, so only the
    // lights' glints on it show, in the specular view).
    if (lobeView()) {
        return blendOutput(in, s, vec4f(s.color, select(s.alpha, 1.0, isWater(in))));
    }

    // Glass and ice: lit, with GL's alpha (more opaque at grazing angles).
    if (!isWater(in)) {
        return blendOutput(in, s, vec4f(applyFog(in, reflectSurface(in, s).color), s.alpha));
    }
    return blendOutput(in, s, waterColour(in, s, t, frontFacing));
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
