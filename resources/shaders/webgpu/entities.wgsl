// Milestone 10a: what the world pass draws besides chunk faces.
//
//   vs_cube / fs_cube   dropped block items: a 0.4-scale cube per item, one
//                       block texture per face (blockEntity.vert/.frag),
//                       instanced from a storage buffer.
//   vs_line / fs_line   GyzmosRenderer's cubes (the block-selection outline)
//                       and lines, as camera-relative line segments
//                       (gyzmosCubeShader / gyzmosLinesShader).
//   vs_model / fs_model skinned models (milestone 10b): players, zombies and
//                       the other creatures, and the player's right hand
//                       (basicEntity.vert/.frag).
//   vs_item / fs_item   item entities and the held item: extruded sprites
//                       and 3D item models (blockEntity.vert, itemEntity.frag).
//
// Entity and item textures are wgpu2d textures, stored top row first, where
// GL's gl2d stored them bottom row first: v is flipped when sampling.
// Cubes, models and items are lit as block faces are (lightEntity, below):
// each carries the light where it stands, as baked, and the shader takes off
// what the time of day does.
//
// Group 0 is the world's bind group (wgpuWorld.cpp); only the bindings used
// here are declared, and they must match chunkFaces.wgsl's.
//
// Milestone 11: cubes, models and items draw into the G-buffer, as GL's
// blockEntity/basicEntity/itemEntity.frag write fboMain's targets: linear
// color, view-space position (roughness 0.9 in its alpha), packed normal,
// no bloom.
// Gizmo lines draw on the screen after the composite, as GL draws them.

#include "world.wgsl"
#include "lighting.wgsl"

@group(0) @binding(3) var<storage, read> textureLayers: array<u32>;
@group(0) @binding(4) var blockTextures: texture_2d_array<f32>;
@group(0) @binding(5) var blockSampler: sampler;

// The G-buffer (chunkFaces.wgsl's GBuffer).
struct GBuffer {
    @location(0) color: vec4f,
    @location(1) position: vec4f, // w: roughness
    @location(2) normal: vec4u,
    @location(3) bloom: vec4f,
};

// GL's normalize(transpose(inverse(m)) * n), for the 3x3 part of m. The
// inverse transpose is the cofactor matrix over the determinant; its columns
// are cross products of m's. Only the determinant's sign matters once the
// result is normalized: the hand is mirrored (negative scale), and dropping
// the sign would turn its normals inward.
fn transformNormal(m: mat4x4f, n: vec3f) -> vec3f {
    let a = m[0].xyz;
    let b = m[1].xyz;
    let c = m[2].xyz;
    let cofactor = mat3x3f(cross(b, c), cross(c, a), cross(a, b));
    return normalize(cofactor * n) * sign(dot(a, cross(b, c)));
}

// The snow weather's whiteout is on entities too (the chunk faces have
// theirs in applyFog), or creatures would stand out of it.
fn gBuffer(color: vec3f, relative: vec3f, normal: vec3f, packedLight: u32) -> GBuffer {
    var out: GBuffer;
    out.color = vec4f(applySnowFog(color, relative, normal, lightLevels(packedLight).skyUnchanged), 1.0);
    out.position = vec4f((u.view * vec4f(relative, 1.0)).xyz, 0.9);
    out.normal = packNormal(normal);
    out.bloom = vec4f(0.0);
    return out;
}

// ---- Dropped block items ----------------------------------------------

struct Cube {
    // xyz the block; w the edge in thousandths of a block, 0 the dropped
    // item's 0.4 (milestone G4's debris sets it).
    positionInt: vec4i,
    // xyz the fraction of the position; w the light where it lies, as baked
    // (sky light << 4 | block light).
    positionFloat: vec4f,
    // Texture slots (BlocksLoader::gpuIds indices) of the six faces: xyzw
    // and slotsB.xy. Milestone G4's debris also uses slotsB.z, the turn (yaw
    // in the low 16 bits, pitch in the high, 0..65535 a full turn), and
    // slotsB.w, flags: bit 0 a quarter of the texture (which one in bits
    // 1-4: u, then v), bit 5 a flat colour instead (rgba8 in slotsA.x).
    slotsA: vec4u,
    slotsB: vec4u,
};

// The 36 vertices of GL's cube entity (renderer.cpp's vertexDataOriginal and
// vertexUVOriginal), 8 floats each: position, normal, uv; 6 per face.
@group(1) @binding(0) var<storage, read> cubeVertices: array<f32>;
@group(1) @binding(1) var<storage, read> cubes: array<Cube>;

struct CubeOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) @interpolate(flat) layer: u32,
    @location(2) @interpolate(flat) light: u32,
    @location(3) relative: vec3f,
    @location(4) normal: vec3f,
    // Milestone G4: a debris piece's flat colour; alpha 0 samples the texture.
    @location(5) @interpolate(flat) colour: vec4f,
};

fn turn(v: vec3f, yaw: f32, pitch: f32) -> vec3f {
    let cp = cos(pitch);
    let sp = sin(pitch);
    let a = vec3f(v.x, v.y * cp - v.z * sp, v.y * sp + v.z * cp);
    let cy = cos(yaw);
    let sy = sin(yaw);
    return vec3f(a.x * cy + a.z * sy, a.y, -a.x * sy + a.z * cy);
}

@vertex
fn vs_cube(@builtin(vertex_index) vertexIndex: u32, @builtin(instance_index) instance: u32) -> CubeOutput {
    let cube = cubes[instance];
    let base = vertexIndex * 8u;
    let shape = vec3f(cubeVertices[base], cubeVertices[base + 1u], cubeVertices[base + 2u]);
    let uv = vec2f(cubeVertices[base + 6u], cubeVertices[base + 7u]);

    let face = vertexIndex / 6u;
    let flags = cube.slotsB.w;
    let flatColour = (flags & 32u) != 0u;
    var slot = cube.slotsA[min(face, 3u)];
    if (face >= 4u) { slot = cube.slotsB[face - 4u]; }
    if (flatColour) { slot = 0u; }

    // A debris piece's own size and turn (milestone G4).
    let size = select(0.4, f32(cube.positionInt.w) / 1000.0, cube.positionInt.w > 0);
    var local = shape * size;
    var normal = vec3f(cubeVertices[base + 3u], cubeVertices[base + 4u], cubeVertices[base + 5u]);
    if (cube.slotsB.z != 0u) {
        let yaw = f32(cube.slotsB.z & 0xFFFFu) / 65535.0 * 6.2831853;
        let pitch = f32(cube.slotsB.z >> 16u) / 65535.0 * 6.2831853;
        local = turn(local, yaw, pitch);
        normal = turn(normal, yaw, pitch);
    }

    // Integer difference first, then the fractions; the cube is scaled by
    // 0.4 about the item's position (GL's u_modelMatrix), or by the piece's
    // size.
    let relative = vec3f(cube.positionInt.xyz - u.positionInt.xyz) - u.positionFloat.xyz
        + cube.positionFloat.xyz + local;

    var out: CubeOutput;
    out.position = psxSnap(u.viewProjection * vec4f(relative, 1.0));
    out.uv = uv;
    if ((flags & 1u) != 0u) {
        let quarter = vec2f(f32((flags >> 1u) & 3u), f32((flags >> 3u) & 3u));
        out.uv = (uv + quarter) * 0.25;
    }
    out.layer = textureLayers[slot] & 0x00FFFFFFu; // the top byte is a block span
    out.light = u32(cube.positionFloat.w);
    out.relative = relative;
    // Scaled evenly and turned as a whole, so the normal is the cube's own.
    out.normal = normal;
    out.colour = select(vec4f(0.0), unpack4x8unorm(cube.slotsA.x), flatColour);
    return out;
}

@fragment
fn fs_cube(in: CubeOutput) -> GBuffer {
    var albedo = textureSample(blockTextures, blockSampler, in.uv, in.layer);
    if (in.colour.a > 0.0) { albedo = vec4f(in.colour.rgb, 1.0); }
    if (albedo.a <= 0.0) { discard; }
    let N = normalize(in.normal);
    return gBuffer(lightEntity(toLinear(albedo.rgb), in.light, in.relative, N, in.position.xy), in.relative, N, in.light);
}

// ---- Gizmo lines --------------------------------------------------------

@vertex
fn vs_line(@location(0) position: vec3f) -> @builtin(position) vec4f {
    // Already camera-relative: the CPU subtracted the camera position.
    return u.viewProjection * vec4f(position, 1.0);
}

@fragment
fn fs_line() -> @location(0) vec4f {
    // gyzmosCubeShader.frag's color.
    return vec4f(0.5, 0.5, 0.6, 1.0);
}

// ---- Skinned models and items (milestone 10b) ----------------------------

// One per model instance (wgpuWorld::ModelInstance).
struct Model {
    // xyz the integer part of the position; w the first bone in `bones`.
    positionInt: vec4i,
    // w: 1 for the first-person hand (perspective kept in the PSX look).
    positionFloat: vec4f,
    // rgb: a linear tint; w: the light where it stands, as baked (sky light
    // << 4 | block light), which lightLevels reads as it does a face's.
    colorAndLight: vec4f,
};

// One per item (wgpuWorld::ItemInstance).
struct Item {
    model: mat4x4f,       // GL's u_modelMatrix
    positionInt: vec4i,   // w: 1 for the held item (perspective kept in the PSX look)
    positionFloat: vec4f, // w: the light where it is, as baked (sky light << 4 | block light)
};

@group(1) @binding(0) var<storage, read> bones: array<mat4x4f>;
@group(1) @binding(1) var<storage, read> models: array<Model>;
@group(1) @binding(2) var<storage, read> items: array<Item>;

// GL's textureSampler0-3: the four textures an entity draws with; each
// vertex names one. Items bind their one texture four times.
@group(2) @binding(0) var entityTexture0: texture_2d<f32>;
@group(2) @binding(1) var entityTexture1: texture_2d<f32>;
@group(2) @binding(2) var entityTexture2: texture_2d<f32>;
@group(2) @binding(3) var entityTexture3: texture_2d<f32>;

struct EntityOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) @interpolate(flat) textureIndex: u32,
    @location(2) @interpolate(flat) brightness: vec3f,
    @location(3) relative: vec3f,
    @location(4) normal: vec3f,
    @location(5) @interpolate(flat) light: u32,
    // The PSX look's uv, without perspective correction (chunkFaces.wgsl's
    // affineUV); entityUV picks.
    @location(6) @interpolate(linear) affineUV: vec2f,
    // 1 for the first-person hand and the held item: they keep their
    // textures in perspective, which would warp hard so close to the eye.
    @location(7) @interpolate(flat) firstPerson: u32,
};

fn entityUV(in: EntityOutput) -> vec2f {
    return select(in.uv, in.affineUV, u.psx.w != 0.0 && in.firstPerson == 0u);
}

struct ModelVertex {
    @location(0) shape: vec3f,
    @location(1) normal: vec3f,
    @location(2) uv: vec2f,
    // model.cpp's Data: short bone, short texture index.
    @location(3) boneAndTexture: vec2i,
};

@vertex
fn vs_model(v: ModelVertex, @builtin(instance_index) instance: u32) -> EntityOutput {
    let m = models[instance];
    let skin = bones[u32(m.positionInt.w) + u32(v.boneAndTexture.x)];

    let relative = vec3f(m.positionInt.xyz - u.positionInt.xyz) - u.positionFloat.xyz
        + m.positionFloat.xyz + (skin * vec4f(v.shape, 1.0)).xyz;

    var out: EntityOutput;
    out.position = psxSnap(u.viewProjection * vec4f(relative, 1.0));
    out.uv = vec2f(v.uv.x, 1.0 - v.uv.y);
    out.affineUV = out.uv;
    out.textureIndex = u32(clamp(v.boneAndTexture.y, 0, 3));
    out.brightness = m.colorAndLight.rgb;
    out.light = u32(m.colorAndLight.w);
    out.firstPerson = u32(m.positionFloat.w);
    out.relative = relative;
    out.normal = transformNormal(skin, v.normal);
    return out;
}

// All four are sampled (textureSample must stay in uniform control flow),
// then the vertex's one is kept.
fn entityAlbedo(in: EntityOutput) -> vec4f {
    let uv = entityUV(in);
    let c0 = textureSample(entityTexture0, blockSampler, uv);
    let c1 = textureSample(entityTexture1, blockSampler, uv);
    let c2 = textureSample(entityTexture2, blockSampler, uv);
    let c3 = textureSample(entityTexture3, blockSampler, uv);
    var c = c0;
    if (in.textureIndex == 1u) { c = c1; }
    if (in.textureIndex == 2u) { c = c2; }
    if (in.textureIndex == 3u) { c = c3; }
    return c;
}

// A model in the world's light, as chunkFaces.wgsl's shade lights a block
// face: the sun or moon (its color and direction from the time of day and the
// weather) with its shadow, the ambient light, and the light blocks. Rough and
// not metal; no ambient occlusion.
fn lightEntity(albedo: vec3f, packedLight: u32, relative: vec3f, N: vec3f, fragCoord: vec2f) -> vec3f {
    let levels = lightLevels(packedLight);
    let viewDepth = -(u.view * vec4f(relative, 1.0)).z;
    let shadow = sunShadow(relative, viewDepth, dot(u.sunDirection.xyz, N), fragCoord);
    let ambient = computeAmbient(withHeldLight(levels, relative, N), N, 1.0, false, 1.0, shadow);

    let roughness = 0.9;
    let F0 = vec3f(0.04);
    let V = -normalize(relative);
    // The block this point is in and where in it (blocks are centered on
    // their integer position), for the point lights.
    let world = u.positionFloat.xyz + relative;
    let cell = floor(world + vec3f(0.5));
    let blockPos = u.positionInt.xyz + vec3i(cell);

    var color = pointLights(levels, blockPos, world - cell, 0.0, roughness, V, albedo, N, F0, vec3f(1.0));
    color += sunLight(levels, 0.0, roughness, V, albedo, N, F0, 1.0, shadow, vec3f(1.0));
    return color + albedo * ambient;
}

@fragment
fn fs_model(in: EntityOutput) -> GBuffer {
    let c = entityAlbedo(in);
    if (c.a < 0.5) { discard; }
    let N = normalize(in.normal);
    let albedo = toLinear(c.rgb) * in.brightness;
    return gBuffer(lightEntity(albedo, in.light, in.relative, N, in.position.xy), in.relative, N, in.light);
}

struct ItemVertex {
    @location(0) shape: vec3f,
    @location(1) normal: vec3f,
    @location(2) uv: vec2f,
};

@vertex
fn vs_item(v: ItemVertex, @builtin(instance_index) instance: u32) -> EntityOutput {
    let item = items[instance];
    let relative = vec3f(item.positionInt.xyz - u.positionInt.xyz) - u.positionFloat.xyz
        + item.positionFloat.xyz + (item.model * vec4f(v.shape, 1.0)).xyz;

    var out: EntityOutput;
    out.position = psxSnap(u.viewProjection * vec4f(relative, 1.0));
    out.uv = vec2f(v.uv.x, 1.0 - v.uv.y);
    out.affineUV = out.uv;
    out.textureIndex = 0u;
    out.brightness = vec3f(1.0);
    out.light = u32(item.positionFloat.w);
    out.firstPerson = u32(item.positionInt.w);
    out.relative = relative;
    out.normal = transformNormal(item.model, v.normal);
    return out;
}

@fragment
fn fs_item(in: EntityOutput) -> GBuffer {
    let c = textureSample(entityTexture0, blockSampler, entityUV(in));
    if (c.a <= 0.0) { discard; }
    let N = normalize(in.normal);
    return gBuffer(lightEntity(toLinear(c.rgb), in.light, in.relative, N, in.position.xy), in.relative, N, in.light);
}
