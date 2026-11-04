// Milestone 10a: what the world pass draws besides chunk faces.
//
//   vs_cube / fs_cube   dropped block items: a 0.4-scale cube per item, one
//                       block texture per face (blockEntity.vert/.frag),
//                       instanced from a storage buffer.
//   vs_line / fs_line   GyzmosRenderer's cubes (the block-selection outline)
//                       and lines, as camera-relative line segments
//                       (gyzmosCubeShader / gyzmosLinesShader).
//
// Group 0 is the world's bind group (wgpuWorld.cpp); only the bindings used
// here are declared, and they must match chunkFaces.wgsl's.

struct Uniforms {
    viewProjection: mat4x4f,
    positionInt: vec4i,
    positionFloat: vec4f,
    waterColor: vec4f,
    planes: vec4f,
};
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(3) var<storage, read> textureLayers: array<u32>;
@group(0) @binding(4) var blockTextures: texture_2d_array<f32>;
@group(0) @binding(5) var blockSampler: sampler;

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
    out.layer = textureLayers[slot];
    out.brightness = cube.positionFloat.w;
    return out;
}

@fragment
fn fs_cube(in: CubeOutput) -> @location(0) vec4f {
    let albedo = textureSample(blockTextures, blockSampler, in.uv, in.layer);
    if (albedo.a <= 0.0) { discard; }
    return vec4f(albedo.rgb * in.brightness, 1.0);
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
