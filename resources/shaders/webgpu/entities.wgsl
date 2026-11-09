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
// Brightness is GL's linear light factor taken to output (sRGB) space, as
// this world is unlit and writes texture colors as they are.
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

fn gBuffer(color: vec3f, relative: vec3f, normal: vec3f) -> GBuffer {
    var out: GBuffer;
    out.color = vec4f(color, 1.0);
    out.position = vec4f((u.view * vec4f(relative, 1.0)).xyz, 0.9);
    out.normal = packNormal(normal);
    out.bloom = vec4f(0.0);
    return out;
}

// ---- Dropped block items ----------------------------------------------

struct Cube {
    positionInt: vec4i,
    // xyz the fraction of the position; w the brightness from the light
    // level (GL's v_ambient, in output space).
    positionFloat: vec4f,
    // Texture slots (BlocksLoader::gpuIds indices) of the six faces.
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
    @location(2) @interpolate(flat) brightness: f32,
    @location(3) relative: vec3f,
    @location(4) normal: vec3f,
};

@vertex
fn vs_cube(@builtin(vertex_index) vertexIndex: u32, @builtin(instance_index) instance: u32) -> CubeOutput {
    let cube = cubes[instance];
    let base = vertexIndex * 8u;
    let shape = vec3f(cubeVertices[base], cubeVertices[base + 1u], cubeVertices[base + 2u]);
    let uv = vec2f(cubeVertices[base + 6u], cubeVertices[base + 7u]);

    let face = vertexIndex / 6u;
    var slot = cube.slotsA[min(face, 3u)];
    if (face >= 4u) { slot = cube.slotsB[face - 4u]; }

    // Integer difference first, then the fractions; the cube is scaled by
    // 0.4 about the item's position (GL's u_modelMatrix).
    let relative = vec3f(cube.positionInt.xyz - u.positionInt.xyz) - u.positionFloat.xyz
        + cube.positionFloat.xyz + shape * 0.4;

    var out: CubeOutput;
    out.position = u.viewProjection * vec4f(relative, 1.0);
    out.uv = uv;
    out.layer = textureLayers[slot] & 0x00FFFFFFu; // the top byte is a block span
    out.brightness = cube.positionFloat.w;
    out.relative = relative;
    // Scaled evenly, so the normal is the cube's own.
    out.normal = vec3f(cubeVertices[base + 3u], cubeVertices[base + 4u], cubeVertices[base + 5u]);
    return out;
}

@fragment
fn fs_cube(in: CubeOutput) -> GBuffer {
    let albedo = textureSample(blockTextures, blockSampler, in.uv, in.layer);
    if (albedo.a <= 0.0) { discard; }
    return gBuffer(toLinear(albedo.rgb * in.brightness), in.relative, normalize(in.normal));
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
    positionFloat: vec4f,
    // rgb: GL's v_color (color * max(light/15, 0.1)), in output space.
    brightness: vec4f,
};

// One per item (wgpuWorld::ItemInstance).
struct Item {
    model: mat4x4f,       // GL's u_modelMatrix
    positionInt: vec4i,
    positionFloat: vec4f, // w: GL's v_ambient, in output space
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
};

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
    out.position = u.viewProjection * vec4f(relative, 1.0);
    out.uv = vec2f(v.uv.x, 1.0 - v.uv.y);
    out.textureIndex = u32(clamp(v.boneAndTexture.y, 0, 3));
    out.brightness = m.brightness.rgb;
    out.relative = relative;
    out.normal = transformNormal(skin, v.normal);
    return out;
}

// All four are sampled (textureSample must stay in uniform control flow),
// then the vertex's one is kept.
fn entityAlbedo(in: EntityOutput) -> vec4f {
    let c0 = textureSample(entityTexture0, blockSampler, in.uv);
    let c1 = textureSample(entityTexture1, blockSampler, in.uv);
    let c2 = textureSample(entityTexture2, blockSampler, in.uv);
    let c3 = textureSample(entityTexture3, blockSampler, in.uv);
    var c = c0;
    if (in.textureIndex == 1u) { c = c1; }
    if (in.textureIndex == 2u) { c = c2; }
    if (in.textureIndex == 3u) { c = c3; }
    return c;
}

@fragment
fn fs_model(in: EntityOutput) -> GBuffer {
    let c = entityAlbedo(in);
    if (c.a < 0.5) { discard; }
    return gBuffer(toLinear(c.rgb * in.brightness), in.relative, normalize(in.normal));
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
    out.position = u.viewProjection * vec4f(relative, 1.0);
    out.uv = vec2f(v.uv.x, 1.0 - v.uv.y);
    out.textureIndex = 0u;
    out.brightness = vec3f(item.positionFloat.w);
    out.relative = relative;
    out.normal = transformNormal(item.model, v.normal);
    return out;
}

@fragment
fn fs_item(in: EntityOutput) -> GBuffer {
    let c = textureSample(entityTexture0, blockSampler, in.uv);
    if (c.a <= 0.0) { discard; }
    return gBuffer(toLinear(c.rgb * in.brightness), in.relative, normalize(in.normal));
}
