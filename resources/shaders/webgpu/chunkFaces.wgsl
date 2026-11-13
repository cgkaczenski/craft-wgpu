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
    // Flag bits: 0 water, 1 in water, 4-7 the AO shape; 8 the top of a cube
    // (not in the face's data: faceVertex sets it), where puddles lie.
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

// How far each flowing-water level (1-7, from the face's shape bits 13-15)
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
fn faceVertex(vertexIndex: u32, face: FaceInput, forShadow: bool) -> VertexOutput {
    // Water faces carry the flowing water's level in bits 13-15, above every
    // orientation: the corner table has over 5000 rows (the models), so the
    // orientation needs the low 13 bits. The attribute is a signed short, so
    // a level of 4 or more arrives negative: read its 16 bits unsigned.
    let orientationBits = u32(face.orientationAndTexture.x) & 0xFFFFu;
    let waterLevel = f32(orientationBits >> 13u);
    let orientation = i32(orientationBits & 0x1FFFu);
    let textureIndex = u32(face.orientationAndTexture.y) & 0x07FFu;
    let colors = (u32(face.orientationAndTexture.y) & 0xF800u) >> 11u;

    // The light and flag bytes ride in y's high bits.
    // y is 0-255; bit 15 is a flag (tuftAboveFlag).
    let blockPosition = vec3i(face.position.x, face.position.y & 0x7FFF, face.position.z);
    let light = u32(face.position.y >> 16) & 0xFFu;
    let flags = (u32(face.position.y >> 24) & 0xFFu) | select(0u, tuftAboveFlag, (face.position.y & 0x8000) != 0);
    let water = (flags & 1u) != 0u;

    let corner = stripToCorner[vertexIndex];
    let facePosition = vec3f(blockPosition);
    var shape = animatedCorner(orientation, corner, facePosition, waterLevel);

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

struct WetGround {
    amount: f32,   // how wet the face is, 0..1
    puddle: f32,   // how much of a puddle this texel is, 0..1
    slope: vec2f,  // the rings' tilt of the puddle's surface, along x and z
    crest: f32,    // a ring's crest (above 0) or the trough beside it (below), -1..1
};

fn groundWet(in: VertexOutput, levels: LightLevels, water: bool) -> WetGround {
    var g: WetGround;
    g.amount = 0.0;
    g.puddle = 0.0;
    g.slope = vec2f(0.0);
    g.crest = 0.0;
    // Not under the snow weather's cover, not water or what is under it,
    // and not what faces down.
    let wetness = u.wet.x * (1.0 - u.snowFog.w);
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

    // The rings, from this cell's drop and its eight neighbours'. Each cell
    // has its own pace; each drop of a cell lands somewhere else in it, and
    // the lighter the rain, the more of them are skipped.
    let rain = u.wet.y;
    if (rain <= 0.0) { return g; }
    let cell = groundCell(texel, ringCell);
    let local = vec2f(texel - cell * ringCell) + 0.5;
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
            g.crest += cos(across * 2.2) * ring;
            g.slope += away / max(distance, 0.001) * sin(across * 2.2) * ring;
        }
    }
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
        // Water has its own color, smooth and not metal.
        albedo = u.waterColor.rgb;
        m.roughness = 0.09;
        m.metallic = 0.0;
    }
    albedo = paintAlbedo(albedo, in.colors);
    if (bladeGroundShaded(in)) { albedo *= bladeGroundShade; }

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
    if (water) { F0 = vec3f(0.021, 0.021, 0.05); }
    F0 = mix(F0, vec3f(0.02), wet.puddle);

    var color = pointLights(levels, in.blockPos, in.inBlock, m.metallic, m.roughness, V, albedo, N, F0, caustics);
    var sunN = N;
    if (isGrass(in)) { sunN = sunNormal; }
    let sun = sunLight(levels, m.metallic, m.roughness, V, albedo, sunN, F0, ao, shadow, caustics);
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
    s.puddle = wet.puddle;
    s.crest = wet.crest;
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
// while the camera is still and right while it moves. Water takes the
// reflection whole (its mix with what is under it comes after); anything
// else mixes it in by its fresnel and roughness. The ray steps by the
// face's own normal for water (GL's v_normal), the mapped one otherwise.
fn reflectSurface(in: VertexOutput, s: Shaded) -> Reflection {
    if (s.puddle > 0.0) { return reflectPuddle(in, s); }
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
    var t = readTexels(in);
    // GL's alpha test: cut-out leaves, grass and flowers. After the prepass
    // this only matters with the prepass off; with it, Equal has already
    // dropped the cut-out pixels' fragments.
    if (t.albedo.a <= 0.0 || spriteDissolved(in) || bladeDissolved(in)) { discard; }
    if (in.blade.x > 0.0) { t.albedo = bladeAlbedo(in, t.albedo); }

    // A blade's normal already faces the camera, whichever way its strip
    // winds.
    let s = shade(in, t, frontFacing || in.blade.x > 0.0);
    var lit = reflectSurface(in, s).color;
    if (in.blade.x > 0.0) { lit *= in.blade.y; }
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
