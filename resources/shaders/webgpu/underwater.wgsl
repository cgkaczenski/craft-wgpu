// Milestone 18: the underwater post pass (Renderer::renderFromBakedData's
// "warp"; postProcess/warp.frag). With the camera under water, the HDR
// color is redrawn into a second texture with its coordinates wobbled by
// sines of the time, tinted 10% toward the underwater color, and the sky
// (no position) painted that color outright. The tone mapper reads that
// texture instead: a pass cannot read the texture it draws into.

struct WarpUniforms {
    // rgb the underWaterColor setting (as GL uses it, unconverted), w unused.
    color: vec4f,
    // x u_time (the renderer's timeGrass), yzw unused.
    params: vec4f,
};
@group(0) @binding(0) var<uniform> warp: WarpUniforms;
@group(0) @binding(1) var colorTexture: texture_2d<f32>;    // the HDR color
@group(0) @binding(2) var positionTexture: texture_2d<f32>; // view space; sky 0
@group(0) @binding(3) var linearSampler: sampler;           // linear, clamped

struct WarpOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
};

@vertex
fn vs_fullscreen(@builtin(vertex_index) index: u32) -> WarpOutput {
    let x = f32((index << 1u) & 2u) * 2.0 - 1.0;
    let y = f32(index & 2u) * 2.0 - 1.0;
    var out: WarpOutput;
    out.position = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f(0.5 + 0.5 * x, 0.5 - 0.5 * y);
    return out;
}

const tau = 6.28318530718;

fn sin01(x: f32) -> f32 {
    return (sin(x * tau) + 1.0) / 2.0;
}

// warp.frag. The wobble runs in GL's coordinates (v up), flipped back to
// sample, so it moves as GL's does.
@fragment
fn fs_warp(in: WarpOutput) -> @location(0) vec4f {
    let t = warp.params.x;
    let amplitude = 0.010;
    let turbulence = 1.5;

    var c = vec2f(in.uv.x, 1.0 - in.uv.y) * 2.0 - 1.0;
    c += vec2f(sin01(c.x * turbulence + t) * amplitude);
    c -= vec2f(sin01(c.y * turbulence + t) * amplitude);
    c = (c + 1.0) / 2.0;
    let uv = vec2f(c.x, 1.0 - c.y);

    let dist = -textureSampleLevel(positionTexture, linearSampler, uv, 0.0).z;
    let color = textureSampleLevel(colorTexture, linearSampler, uv, 0.0).rgb;
    if (dist <= 0.001) { return vec4f(warp.color.rgb, 1.0); }
    return vec4f(mix(color, warp.color.rgb, 0.1), 1.0);
}
