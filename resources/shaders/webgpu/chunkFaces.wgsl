// Milestone 6: chunk faces, untextured. The WebGPU counterpart of the
// position half of rendering/defaultShader.vert.
//
// One instance per block face, read from the face arena with instance step
// mode: the same 16 bytes Chunk::bake writes for OpenGL (bigGpuBuffer.cpp's
// setupVertexAttributes):
//   bytes 0-1  orientation (short): which corner table row, 0-5 cube faces,
//              6-9 grass, 10-15 leaves, then models
//   bytes 2-3  texture index (short): low 11 bits the texture, high 5 colors
//   bytes 4-15 block position (ivec3, world blocks); y packs light in bits
//              16-23 and flags in 24-31
//
// The four corners come from vertexData, the table Renderer builds in
// recreateBlockGeometryData: 4 corners x 3 floats per orientation. OpenGL
// drew them as a triangle fan of indices 0,1,2,3; WebGPU has no fans, so a
// 4-vertex triangle strip visits them as 0,1,3,2 -- the same quad, the same
// counter-clockwise winding.
//
// Conventions (docs/roadmap.md): positions camera-relative, depth 0..1.
// No animation yet (grass, leaves and water sway in defaultShader.vert).

struct Uniforms {
    // perspectiveRH_ZO times Camera::getViewMatrix (rotation only).
    viewProjection: mat4x4f,
    // The camera position split as in Camera::decomposePosition, the
    // u_positionInt / u_positionFloat pair. w unused.
    positionInt: vec4i,
    positionFloat: vec4f,
};
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read> vertexData: array<f32>;

struct FaceInput {
    @location(0) orientationAndTexture: vec2i,
    @location(1) position: vec3i,
};

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) color: vec3f,
};

// defaultShader.vert's vertexColor: a fixed darkening per orientation (front,
// back, top, bottom, left, right; grass; leaves), standing in for lighting.
const vertexColor = array<f32, 16>(
    0.95, 0.85, 1.0, 0.8, 0.85, 0.95,
    0.95, 0.95, 0.95, 0.95,
    0.95, 0.85, 1.0, 0.8, 0.85, 0.95,
);

const stripToCorner = array<u32, 4>(0u, 1u, 3u, 2u);

// A flat color per texture until milestone 7 samples the real ones, so
// different blocks are told apart.
fn textureColor(textureIndex: u32) -> vec3f {
    var h = textureIndex * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    let rgb = vec3f(f32(h & 255u), f32((h >> 8u) & 255u), f32((h >> 16u) & 255u)) / 255.0;
    return 0.35 + 0.55 * rgb;
}

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex: u32, face: FaceInput) -> VertexOutput {
    let orientation = face.orientationAndTexture.x;
    let textureIndex = u32(face.orientationAndTexture.y) & 0x07FFu;

    // The light and flag bytes ride in y's high bits.
    let blockPosition = vec3i(face.position.x, face.position.y & 0xFFFF, face.position.z);

    // Leaves (10-15) reuse the cube faces' rows; defaultShader.vert only
    // adds their sway on top.
    var row = orientation;
    if (orientation >= 10 && orientation < 16) { row = orientation - 10; }

    let corner = stripToCorner[vertexIndex];
    let base = u32(row) * 12u + corner * 3u;
    let shape = vec3f(vertexData[base], vertexData[base + 1u], vertexData[base + 2u]);

    // Integer difference first, then the fraction: exact however far the
    // world is from the origin.
    let relative = vec3f(blockPosition - u.positionInt.xyz) - u.positionFloat.xyz + shape;

    var shade = 1.0;
    if (orientation >= 0 && orientation < 16) { shade = vertexColor[orientation]; }

    var out: VertexOutput;
    out.position = u.viewProjection * vec4f(relative, 1.0);
    out.color = textureColor(textureIndex) * shade;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    return vec4f(in.color, 1.0);
}
