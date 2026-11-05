// Milestone 11: turns one G-buffer attachment into something a screen can
// show. Two uses, one shader:
//   - the composite: the HDR color drawn onto the surface at the end of the
//     world (`view` 0), converted from linear light back to sRGB. The post
//     chain (milestone 15) replaces this with tone mapping and FXAA.
//   - the debug views: each attachment drawn into a small wgpu2d target that
//     the ImGui "G-buffer" header shows (wgpuWorld::gBufferPreview).
//
// A fullscreen triangle: three vertices, no buffers, covering the target. Its
// uv runs 0..1 across the target with v down (render targets are top-first),
// so a texel is found as uv * the attachment's size, whatever the size of
// the target being drawn. Texels are loaded, not sampled: the normals are
// integers, and the other attachments are shown as they are.

@group(0) @binding(0) var colorTexture: texture_2d<f32>;    // rg11b10ufloat, linear
@group(0) @binding(1) var positionTexture: texture_2d<f32>; // rgba16float, view space; w roughness
@group(0) @binding(2) var normalTexture: texture_2d<u32>;   // rgba16uint, packed
@group(0) @binding(3) var bloomTexture: texture_2d<f32>;    // rg11b10ufloat, linear
@group(0) @binding(4) var depthTexture: texture_depth_2d;   // depth32float, 0..1

// The world's uniforms (chunkFaces.wgsl's Uniforms), for the camera planes.
struct Uniforms {
    viewProjection: mat4x4f,
    positionInt: vec4i,
    positionFloat: vec4f,
    waterColor: vec4f,
    planes: vec4f, // x near, y far
};
@group(0) @binding(5) var<uniform> u: Uniforms;

// Which view, in wgpuWorld.cpp's gBufferViewLabels order: color, position,
// normal, bloom, roughness, depth.
override view: u32 = 0u;

struct ViewOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
};

@vertex
fn vs_fullscreen(@builtin(vertex_index) index: u32) -> ViewOutput {
    // (-1,-1), (3,-1), (-1,3): one triangle whose inside covers the screen.
    let x = f32((index << 1u) & 2u) * 2.0 - 1.0;
    let y = f32(index & 2u) * 2.0 - 1.0;
    var out: ViewOutput;
    out.position = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f(0.5 + 0.5 * x, 0.5 - 0.5 * y);
    return out;
}

fn fromLinear(c: vec3f) -> vec3f {
    let low = c * 12.92;
    let high = 1.055 * pow(c, vec3f(1.0 / 2.4)) - 0.055;
    return select(high, low, c < vec3f(0.0031308));
}

fn texelOf(uv: vec2f, size: vec2u) -> vec2i {
    return min(vec2i(uv * vec2f(size)), vec2i(size) - 1);
}

@fragment
fn fs_view(in: ViewOutput) -> @location(0) vec4f {
    let texel = texelOf(in.uv, textureDimensions(colorTexture));

    if (view == 1u) {
        // View-space position: x right, y up, z toward the camera, so the
        // scene ahead is -z. Each axis as a channel, 32 blocks to full.
        let p = textureLoad(positionTexture, texel, 0).xyz;
        return vec4f(clamp(abs(p) / 32.0, vec3f(0.0), vec3f(1.0)), 1.0);
    }
    if (view == 2u) {
        // The packed normal back to -1..1, shown as 0..1.
        let n = vec3f(textureLoad(normalTexture, texel, 0).xyz) / 65536.0 * 2.0 - 1.0;
        return vec4f(n * 0.5 + 0.5, 1.0);
    }
    if (view == 3u) {
        return vec4f(fromLinear(clamp(textureLoad(bloomTexture, texel, 0).rgb, vec3f(0.0), vec3f(1.0))), 1.0);
    }
    if (view == 4u) {
        // GL's fifth target, carried in the positions' alpha.
        return vec4f(vec3f(textureLoad(positionTexture, texel, 0).w), 1.0);
    }
    if (view == 5u) {
        // Depth as distance, square-rooted so the near blocks are not all
        // black: white at the far plane (and where nothing was drawn).
        let d = textureLoad(depthTexture, texel, 0);
        let near = u.planes.x;
        let far = u.planes.y;
        let distance = near * far / (far - d * (far - near));
        return vec4f(vec3f(sqrt(clamp(distance / far, 0.0, 1.0))), 1.0);
    }
    return vec4f(fromLinear(clamp(textureLoad(colorTexture, texel, 0).rgb, vec3f(0.0), vec3f(1.0))), 1.0);
}
