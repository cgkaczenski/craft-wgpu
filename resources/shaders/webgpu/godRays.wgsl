// Milestone 18: screen-space god rays (renderSun's "SSGR" region;
// postProcess/maskDepth.frag and radialBlur.frag). At half the surface's
// size, after sky.wgsl's fs_sunRays has drawn the sun and moon there:
//
//   fs_mask        black wherever the scene has depth (anything drawn), so
//                  only the parts of the sun and moon over the sky are left.
//   fs_radialBlur  200 taps from each pixel toward the sun's position on
//                  screen, falling off linearly, the length modulated by a
//                  sine of the angle to make 20 rays.
//
// bloom.wgsl's fs_apply adds the result, as GL's applyBloomData does.

struct RayUniforms {
    // xy the sun's position on screen in GL's texture coordinates (v up):
    // its clip position's xy / w * 0.5 + 0.5. zw unused.
    center: vec4f,
};
@group(0) @binding(0) var<uniform> rays: RayUniforms;
@group(0) @binding(1) var source: texture_2d<f32>;      // the masked sun and moon
@group(0) @binding(2) var sourceSampler: sampler;       // linear, clamped
@group(0) @binding(3) var sceneDepth: texture_depth_2d; // full size, 0..1

struct RayOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
};

// drawQuads.vert's quad as one triangle; uv 0..1 across the target, v down.
@vertex
fn vs_fullscreen(@builtin(vertex_index) index: u32) -> RayOutput {
    let x = f32((index << 1u) & 2u) * 2.0 - 1.0;
    let y = f32(index & 2u) * 2.0 - 1.0;
    var out: RayOutput;
    out.position = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f(0.5 + 0.5 * x, 0.5 - 0.5 * y);
    return out;
}

// maskDepth.frag: GL reads the depth at the half-size pixel's coordinate;
// here the full-size texel under it. The sky (depth 1) is left alone.
@fragment
fn fs_mask(in: RayOutput) -> @location(0) vec4f {
    let size = textureDimensions(sceneDepth);
    let texel = min(vec2u(in.uv * vec2f(size)), size - 1u);
    if (textureLoad(sceneDepth, texel, 0) >= 1.0) { discard; }
    return vec4f(0.0);
}

// radialBlur.frag. It runs in GL's coordinates (v up) and flips v only to
// sample: the rays' pattern is a sine of the angle toward the sun, which a
// flipped v would mirror.
@fragment
fn fs_radialBlur(in: RayOutput) -> @location(0) vec4f {
    let numSamples = 200;
    let blurStrength = 0.6;
    let rayFrequency = 20.0;
    let rayAmplitude = 0.1;

    let coord = vec2f(in.uv.x, 1.0 - in.uv.y);
    let direction = normalize(rays.center.xy - coord);
    let angle = atan2(direction.y, direction.x);
    let rayModulation = 1.0 + rayAmplitude * sin(angle * rayFrequency);

    var color = vec4f(0.0);
    var totalWeight = 0.0;
    for (var i = 0; i < numSamples; i++) {
        let t = f32(i) / f32(numSamples);
        let p = coord + direction * t * blurStrength * rayModulation;
        color += textureSampleLevel(source, sourceSampler, vec2f(p.x, 1.0 - p.y), 0.0) * (1.0 - t);
        totalWeight += 1.0 - t;
    }
    return color / totalWeight;
}
