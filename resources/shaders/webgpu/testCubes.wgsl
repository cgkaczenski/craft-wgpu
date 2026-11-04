// Milestone 5: the first 3D draw. Two unit cubes near where the player
// started, drawn with the game's camera, a depth buffer and back-face culling.
//
// There is no vertex buffer. Each cube is 36 vertices (6 faces x 2
// triangles), and the vertex shader builds its own corner from
// @builtin(vertex_index), the way chunk faces will in milestone 6. The two
// cubes are two instances of the same 36 vertices.
//
// Conventions (docs/roadmap.md, "Conventions"): clip-space y up, depth 0..1
// with the near plane at 0, positions relative to the camera.

struct Uniforms {
    // Projection (perspectiveRH_ZO) times the camera's rotation-only view
    // matrix. The translation is not in it: positions arrive camera-relative.
    viewProjection: mat4x4f,
    // Each cube's min corner minus the camera position, computed in doubles
    // on the CPU. w unused.
    cubeOffsets: array<vec4f, 2>,
};
@group(0) @binding(0) var<uniform> u: Uniforms;

// Four corners per face, counter-clockwise seen from outside the cube, so
// with frontFace Ccw and cullMode Back only the outside of each face draws.
const faceCorners = array<vec3f, 24>(
    vec3f(1, 0, 0), vec3f(1, 1, 0), vec3f(1, 1, 1), vec3f(1, 0, 1), // +x
    vec3f(0, 0, 0), vec3f(0, 0, 1), vec3f(0, 1, 1), vec3f(0, 1, 0), // -x
    vec3f(0, 1, 0), vec3f(0, 1, 1), vec3f(1, 1, 1), vec3f(1, 1, 0), // +y
    vec3f(0, 0, 0), vec3f(1, 0, 0), vec3f(1, 0, 1), vec3f(0, 0, 1), // -y
    vec3f(0, 0, 1), vec3f(1, 0, 1), vec3f(1, 1, 1), vec3f(0, 1, 1), // +z
    vec3f(0, 0, 0), vec3f(0, 1, 0), vec3f(1, 1, 0), vec3f(1, 0, 0), // -z
);

// Two triangles out of a face's four corners: (0, 1, 2) and (0, 2, 3).
const triangleCorner = array<u32, 6>(0, 1, 2, 0, 2, 3);

// One color per face, so a screenshot says which faces are showing:
// x red, y green, z blue; the positive side bright, the negative side dark.
const faceColors = array<vec3f, 6>(
    vec3f(0.9, 0.2, 0.2), vec3f(0.4, 0.05, 0.05),
    vec3f(0.2, 0.9, 0.2), vec3f(0.05, 0.4, 0.05),
    vec3f(0.2, 0.3, 0.95), vec3f(0.05, 0.1, 0.4),
);

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) color: vec3f,
};

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex: u32,
           @builtin(instance_index) instanceIndex: u32) -> VertexOutput {
    let face = vertexIndex / 6u;
    let corner = faceCorners[face * 4u + triangleCorner[vertexIndex % 6u]];
    let relative = u.cubeOffsets[instanceIndex].xyz + corner;

    var out: VertexOutput;
    out.position = u.viewProjection * vec4f(relative, 1.0);
    // The second cube is paler, to tell the two apart where they cross.
    out.color = mix(faceColors[face], vec3f(1.0), f32(instanceIndex) * 0.45);
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    return vec4f(in.color, 1.0);
}
