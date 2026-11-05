// Milestone 16: bloom and the auto exposure's average (Renderer::renderFromBakedData's
// "get automatic exposure", "get bloom filtered data", "bloom blur" and the
// post process's "bloom"; postProcess/filterBloomData.frag, filterDown.frag,
// addMipsShader.frag, gausianBlur.frag, applyBloomData.frag).
//
//   fs_filter    the HDR color's bright pixels, added into the G-buffer's
//                bloom target (blend One, One) beside what the blocks put there.
//   fs_down      Call of Duty's 13-tap downsample, into the next blur level.
//   fs_tent      a 9-tap tent (GL's addMips, divided by 12, not 16): the copy
//                at each level, and the upsample (blend One, One).
//   fs_blur      a 9-tap Gaussian, horizontal or vertical (the override).
//   fs_apply     the blurred bloom and its lens dirt, added onto the HDR color.
//   fs_average   one level of the exposure's average: each texel the mean of
//                the 2x2 under it (glGenerateMipmap's box filter).
//
// Each pass is a fullscreen triangle reading `source`, a view of one mip, so
// GL's textureLod(u_texture, uv, u_mip) is level 0 here and its
// textureSize(u_texture, u_mip) is textureDimensions(source).
//
// GL clamps the blur textures to a black border; WebGPU has no border
// color, so tap() makes reads outside 0..1 black instead (GL also fades
// the outermost half texel toward black; this does not).

struct BloomUniforms {
    // x u_exposure (the adaptive exposure plus the setting), y u_tresshold,
    // z u_multiplier (already / 10000), w unused.
    params: vec4f,
    // x u_hitIntensity (getHitLensDirt), y u_waterDropsPower, zw unused.
    dirt: vec4f,
};
@group(0) @binding(0) var<uniform> bloom: BloomUniforms;
@group(0) @binding(1) var source: texture_2d<f32>;
@group(0) @binding(2) var sourceSampler: sampler; // linear, clamped

struct BloomOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
};

// drawQuads.vert's quad as one triangle; uv 0..1 across the target, v down.
// GL's v runs up, but its kernels are symmetric, so nothing changes.
@vertex
fn vs_fullscreen(@builtin(vertex_index) index: u32) -> BloomOutput {
    let x = f32((index << 1u) & 2u) * 2.0 - 1.0;
    let y = f32(index & 2u) * 2.0 - 1.0;
    var out: BloomOutput;
    out.position = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f(0.5 + 0.5 * x, 0.5 - 0.5 * y);
    return out;
}

fn reinhard(v: vec3f) -> vec3f {
    return v / (1.0 + v);
}

// A read of the blur source, black outside it (GL's border color).
fn tap(uv: vec2f) -> vec3f {
    let inside = all(uv >= vec2f(0.0)) && all(uv <= vec2f(1.0));
    return select(vec3f(0.0), textureSampleLevel(source, sourceSampler, uv, 0.0).rgb, inside);
}

// filterBloomData.frag: what is bright enough, tone mapped and scaled down.
@fragment
fn fs_filter(in: BloomOutput) -> @location(0) vec4f {
    let c = textureSampleLevel(source, sourceSampler, in.uv, 0.0).rgb;
    let lightIntensity = dot(c, vec3f(0.2126, 0.7152, 0.0722));
    let threshold = bloom.params.y;
    if (lightIntensity * bloom.params.x > threshold) {
        return vec4f(reinhard(c) * bloom.params.z * (1.0 / threshold) * 1.2, 1.0);
    }
    return vec4f(0.0, 0.0, 0.0, 1.0);
}

// filterDown.frag: five overlapping 2x2 boxes, the middle one weighted most.
@fragment
fn fs_down(in: BloomOutput) -> @location(0) vec4f {
    let texel = 1.0 / vec2f(textureDimensions(source));
    let texel2 = texel * 2.0;
    let uv = in.uv;

    let topLeft = tap(uv + texel2 * vec2f(-1.0, 1.0));
    let topMid = tap(uv + texel2 * vec2f(0.0, 1.0));
    let topRight = tap(uv + texel2 * vec2f(1.0, 1.0));
    let midLeft = tap(uv + texel2 * vec2f(-1.0, 0.0));
    let midMid = tap(uv);
    let midRight = tap(uv + texel2 * vec2f(1.0, 0.0));
    let bottomLeft = tap(uv + texel2 * vec2f(-1.0, -1.0));
    let bottomMid = tap(uv + texel2 * vec2f(0.0, -1.0));
    let bottomRight = tap(uv + texel2 * vec2f(1.0, -1.0));

    let center1 = tap(uv + texel * vec2f(-1.0, 1.0));
    let center2 = tap(uv + texel * vec2f(1.0, 1.0));
    let center3 = tap(uv + texel * vec2f(1.0, -1.0));
    let center4 = tap(uv + texel * vec2f(-1.0, -1.0));

    let midKernel = 0.5 * (center1 + center2 + center3 + center4) * 0.25;
    let topLeftKernel = 0.125 * (topLeft + topMid + midLeft + midMid) * 0.25;
    let topRightKernel = 0.125 * (topRight + topMid + midRight + midMid) * 0.25;
    let bottomLeftKernel = 0.125 * (bottomLeft + bottomMid + midLeft + midMid) * 0.25;
    let bottomRightKernel = 0.125 * (bottomRight + bottomMid + midRight + midMid) * 0.25;
    return vec4f(midKernel + topLeftKernel + topRightKernel + bottomLeftKernel + bottomRightKernel, 1.0);
}

// addMipsShader.frag: a 3x3 tent (1 2 1 / 2 4 2 / 1 2 1) over GL's 12, so
// each use brightens by 4/3, as GL's does.
@fragment
fn fs_tent(in: BloomOutput) -> @location(0) vec4f {
    let texel = 1.0 / vec2f(textureDimensions(source));
    let uv = in.uv;
    var c = tap(uv + texel * vec2f(-1.0, 1.0));
    c += tap(uv + texel * vec2f(0.0, 1.0)) * 2.0;
    c += tap(uv + texel * vec2f(1.0, 1.0));
    c += tap(uv + texel * vec2f(-1.0, 0.0)) * 2.0;
    c += tap(uv) * 4.0;
    c += tap(uv + texel * vec2f(1.0, 0.0)) * 2.0;
    c += tap(uv + texel * vec2f(-1.0, -1.0));
    c += tap(uv + texel * vec2f(0.0, -1.0)) * 2.0;
    c += tap(uv + texel * vec2f(1.0, -1.0));
    return vec4f(c / 12.0, 1.0);
}

// gausianBlur.frag. GL's u_texel is 1 / the level's size, which is the
// source's size here (the source and the target are the same level).
// GL's weight array and loop are written out: naga's override pass rejects
// the array (ExpressionAlreadyInScope, as with milestone 13's noise).
override horizontal: bool = true;

fn blurPair(uv: vec2f, offset: vec2f, weight: f32) -> vec3f {
    return (tap(uv + offset) + tap(uv - offset)) * weight;
}

@fragment
fn fs_blur(in: BloomOutput) -> @location(0) vec4f {
    let texel = 1.0 / vec2f(textureDimensions(source));
    var offset = vec2f(0.0, texel.y);
    if (horizontal) { offset = vec2f(texel.x, 0.0); }
    var c = tap(in.uv) * 0.227027;
    c += blurPair(in.uv, offset, 0.1945946);
    c += blurPair(in.uv, offset * 2.0, 0.1216216);
    c += blurPair(in.uv, offset * 3.0, 0.054054);
    c += blurPair(in.uv, offset * 4.0, 0.016216);
    return vec4f(c, 1.0);
}

// applyBloomData.frag, with milestone 18's god rays (godRays.wgsl's
// half-size result, GL's u_SSGR) and the depth they are weighed by.
@group(0) @binding(3) var lensDirt: texture_2d<f32>;
@group(0) @binding(4) var hitDirt: texture_2d<f32>;
@group(0) @binding(5) var waterDirt: texture_2d<f32>;
@group(0) @binding(6) var dirtSampler: sampler; // nearest (gl2d's pixelated)
@group(0) @binding(7) var godRays: texture_2d<f32>;
@group(0) @binding(8) var sceneDepth: texture_depth_2d;

fn addLensBlur(c: vec3f, presence: f32, strength: f32) -> vec3f {
    return reinhard(c * presence) * strength;
}

@fragment
fn fs_apply(in: BloomOutput) -> @location(0) vec4f {
    // The dirt textures straight, v down: gl2d stores rows bottom first and
    // GL's v runs up, wgpu2d stores them top first and v runs down, so both
    // put the image's top row at the top of the screen.
    let dirtUv = in.uv;
    var c = textureSample(source, sourceSampler, in.uv).rgb;
    let dirtIntensity = textureSample(lensDirt, dirtSampler, dirtUv).rgb;
    let hitIntensity = textureSample(hitDirt, dirtSampler, dirtUv).rgb;
    let waterIntensity = textureSample(waterDirt, dirtSampler, dirtUv).rgb;
    let ssgr = textureSample(godRays, sourceSampler, in.uv).rgb;
    let depthSize = textureDimensions(sceneDepth);
    let depth = textureLoad(sceneDepth, min(vec2u(in.uv * vec2f(depthSize)), depthSize - 1u), 0);

    c = min(c, vec3f(2.0));
    c += addLensBlur(dirtIntensity * c, 1.5, 2.1);
    c += addLensBlur(hitIntensity * c, bloom.dirt.x * 8.0, 5.0);
    c += addLensBlur(waterIntensity * c, 6.0 * bloom.dirt.y, 4.0);
    // The rays: squared (fainter) over the sky, as they are over geometry
    // linearly.
    if (depth == 1.0) {
        c += pow(ssgr * 0.4, vec3f(2.0));
    } else {
        c += ssgr * 0.4;
    }
    return vec4f(c, 1.0);
}

// The exposure's average: one level down. A linear read at the middle of
// a target texel lands on the corner of the 2x2 under it, so the four are
// averaged evenly (for odd sizes, nearly so).
@fragment
fn fs_average(in: BloomOutput) -> @location(0) vec4f {
    return vec4f(textureSampleLevel(source, sourceSampler, in.uv, 0.0).rgb, 1.0);
}
