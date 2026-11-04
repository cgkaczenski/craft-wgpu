// Milestone 4: Dear ImGui's geometry (from space-game-wgpu, unchanged).
//
// ImGui hands the backend one vertex format only, ImDrawVert, 20 bytes:
// position (2 floats), texture coordinate (2 floats), color (one packed
// RGBA8 word). The pipeline reads the color as Unorm8x4, so the shader sees
// it already unpacked into 0..1 floats at location 2.
//
// Positions are screen points with the origin at DisplayPos, y down. That
// is the same handedness as the game's world pixels, but there is no camera
// here: the projection is a fixed orthographic matrix over the viewport,
// rewritten once per frame on the CPU. On a Retina display these points are
// half the framebuffer pixels; clip space is normalized, so only the
// scissor rectangles (recorded on the CPU) care about that scale.
//
// Colors are ImGui's own non-linear sRGB values, written straight to the
// non-sRGB surface, exactly as imgui_impl_opengl3 writes them to the default
// framebuffer. No gamma conversion, so the two builds match.

struct VertexInput {
    @location(0) position: vec2f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
};

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) color: vec4f,
    @location(1) uv: vec2f,
};

// Group 0 is the sprite pipeline's texture layout, reused as-is: whatever
// ImTextureID a draw command carries is a wgpu2d::Texture id, and the
// texture registry already built this group for it: the font atlas, or a
// texture the game passes to ImGui::Image.
@group(0) @binding(0) var uiTexture: texture_2d<f32>;
@group(0) @binding(1) var uiSampler: sampler;

// Group 1 is this pipeline's own uniform: no camera, no dynamic offset,
// just the viewport projection.
struct Uniforms {
    projection: mat4x4f,
};
@group(1) @binding(0) var<uniform> uniforms: Uniforms;

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    out.position = uniforms.projection * vec4f(in.position, 0.0, 1.0);
    out.color = in.color;
    out.uv = in.uv;
    return out;
}

// The font atlas is an alpha mask in RGBA form: white texels with the
// coverage in alpha. Multiplying by the vertex color tints text and, for
// the solid white texel every filled rectangle samples, leaves the color
// untouched.
@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    return in.color * textureSample(uiTexture, uiSampler, in.uv);
}
