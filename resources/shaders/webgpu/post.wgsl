// Milestone 15: the post chain's last two passes (Renderer::renderFromBakedData's
// "tone mapping" and "fxaa"; postProcess/toneMap.frag and fxaa.frag).
//
//   fs_toneMap  the HDR color to display values: exposure, GL's color
//               adjustments, the chosen tone mapper, the sRGB curve, a dither
//               and the vignette. Into the FXAA input when FXAA is on (GL's
//               fboCoppy, rg11b10ufloat like it), else onto the surface.
//   fs_fxaa     FXAA 3.11 as GL has it (Simon Rodriguez's write-up): edges
//               found by luma, then blended along them. Onto the surface.
//
// Each is a fullscreen triangle reading `source`. Every read is
// textureSampleLevel: FXAA branches and loops on what it reads, and
// textureSample (which takes derivatives) is only allowed in uniform
// control flow. There is one mip, so level 0 is what GL's texture() reads.

struct PostUniforms {
    // x u_exposure, y u_saturation, z u_vibrance, w u_gamma.
    grade: vec4f,
    // x u_shadowBoost, y u_highlightBoost, z u_vignette, w unused.
    boost: vec4f,
    // rgb u_lift (already * 0.2 - 0.1), u_gain (already * 0.4 + 0.8).
    lift: vec4f,
    gain: vec4f,
    // x u_tonemapper: 0 ACES, 1 AgX, 2 ZCAM, 3 Uncharted 2, 4 PBR Neutral.
    mode: vec4i,
    // Milestone G2, the screen effects (rendering/postModifiers.h), all
    // identity when none runs. Colours are display values, amounts 0..1:
    // the tint keeps the brightness, the fade is the colour itself, the
    // edges colour the screen's edges.
    tint: vec4f,
    fade: vec4f,
    edges: vec4f,
    // x blur radius, y radial blur 0..1, zw the double vision's ghost
    // offset; pixels at 1080 lines.
    scene: vec4f,
    // x contrast around mid grey.
    look: vec4f,
};
@group(0) @binding(0) var<uniform> post: PostUniforms;
@group(0) @binding(1) var source: texture_2d<f32>;
@group(0) @binding(2) var sourceSampler: sampler; // linear, clamped (GL's FBO textures)

struct PostOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
};

// drawQuads.vert's quad as one triangle; uv 0..1 across the target, v down
// (both the image and the target are top-first).
@vertex
fn vs_fullscreen(@builtin(vertex_index) index: u32) -> PostOutput {
    let x = f32((index << 1u) & 2u) * 2.0 - 1.0;
    let y = f32(index & 2u) * 2.0 - 1.0;
    var out: PostOutput;
    out.position = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f(0.5 + 0.5 * x, 0.5 - 0.5 * y);
    return out;
}

// ---- Tone mappers (toneMap.frag) ------------------------------------------

// ACES, Stephen Hill's fit (via MJP's BakingLab). GLSL and WGSL both build
// matrices from columns, so the same numbers and the same transposes.
const acesInput = mat3x3f(
    0.59719, 0.35458, 0.04823,
    0.07600, 0.90834, 0.01566,
    0.02840, 0.13383, 0.83777,
);
const acesOutput = mat3x3f(
     1.60475, -0.53108, -0.07367,
    -0.10208,  1.10813, -0.00605,
    -0.00327, -0.07276,  1.07602,
);

fn rrtAndOdtFit(v: vec3f) -> vec3f {
    let a = v * (v + 0.0245786) - 0.000090537;
    let b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

fn acesFitted(color: vec3f) -> vec3f {
    var c = transpose(acesInput) * color;
    c = rrtAndOdtFit(c);
    c = transpose(acesOutput) * c;
    return clamp(c, vec3f(0.0), vec3f(1.0));
}

// AgX with the "punchy" look (GL's AGX_LOOK 2). Its output is already for
// display: no sRGB curve after it.
fn agx(color: vec3f) -> vec3f {
    var col = mat3x3f(0.842, 0.0423, 0.0424, 0.0784, 0.878, 0.0784, 0.0792, 0.0792, 0.879) * color;
    col = clamp((log2(col) + 12.47393) / 16.5, vec3f(0.0), vec3f(1.0));
    col = 0.5 + 0.5 * sin(((-3.11 * col + 6.42) * col - 0.378) * col - 1.44);
    return mix(vec3f(dot(col, vec3f(0.216, 0.7152, 0.0722))), pow(col, vec3f(1.35)), 1.4);
}

// ZCAM (shadertoy dlGBDD): lightness compressed in a perceptual space, then
// chroma pulled back into the sRGB gamut by a binary search.
const zcamLp = 10000.0;
const zcamM1 = 2610.0 / 16384.0;
const zcamM2 = 1.7 * 2523.0 / 32.0;
const zcamC1 = 107.0 / 128.0;
const zcamC2 = 2413.0 / 128.0;
const zcamC3 = 2392.0 / 128.0;
const zcamW = 140.0;
const zcamB = 1.15;
const zcamG = 0.66;

fn eotfPq(v: vec3f) -> vec3f {
    var x = sign(v) * pow(abs(v), vec3f(1.0 / zcamM2));
    x = sign(x) * pow((abs(x) - zcamC1) / (zcamC2 - zcamC3 * abs(x)), vec3f(1.0 / zcamM1)) * zcamLp;
    return x;
}

fn eotfPqInverse(v: vec3f) -> vec3f {
    var x = v / zcamLp;
    x = sign(x) * pow(abs(x), vec3f(zcamM1));
    x = sign(x) * pow((zcamC1 + zcamC2 * abs(x)) / (1.0 + zcamC3 * abs(x)), vec3f(zcamM2));
    return x;
}

fn xyzToICh(xyzIn: vec3f) -> vec3f {
    var xyz = xyzIn * zcamW;
    let bg = vec2f(zcamB, zcamG);
    xyz = vec3f(bg * xyz.xy - (bg - 1.0) * xyz.zx, xyz.z);
    let xyzToLms = transpose(mat3x3f(
         0.41479,   0.579999, 0.014648,
        -0.20151,   1.12065,  0.0531008,
        -0.0166008, 0.2648,   0.66848));
    var lms = xyzToLms * xyz;
    lms = eotfPqInverse(lms);
    let lmsToIab = transpose(mat3x3f(
        0.0,       1.0,      0.0,
        3.524,    -4.06671,  0.542708,
        0.199076,  1.0968,  -1.29588));
    let iab = lmsToIab * lms;
    let i = eotfPq(vec3f(iab.x)).x / zcamW;
    let c = length(iab.yz);
    let h = atan2(iab.z, iab.y);
    return vec3f(i, c, h);
}

fn iChToXyz(iCh: vec3f) -> vec3f {
    var iab: vec3f;
    iab.x = eotfPqInverse(vec3f(iCh.x * zcamW)).x;
    iab.y = iCh.y * cos(iCh.z);
    iab.z = iCh.y * sin(iCh.z);
    let iabToLms = transpose(mat3x3f(
        1.0, 0.2772,  0.1161,
        1.0, 0.0,     0.0,
        1.0, 0.0426, -0.7538));
    var lms = iabToLms * iab;
    lms = eotfPq(lms);
    let lmsToXyz = transpose(mat3x3f(
         1.92423, -1.00479,  0.03765,
         0.35032,  0.72648, -0.06538,
        -0.09098, -0.31273,  1.52277));
    var xyz = lmsToXyz * lms;
    xyz.x = (xyz.x + (zcamB - 1.0) * xyz.z) / zcamB;
    xyz.y = (xyz.y + (zcamG - 1.0) * xyz.x) / zcamG;
    return xyz / zcamW;
}

const xyzToSrgb = mat3x3f(
     3.2404542, -0.9692660,  0.0556434,
    -1.5371385,  1.8760108, -0.2040259,
    -0.4985314,  0.0415560,  1.0572252);
const srgbToXyz = mat3x3f(
    0.4124564, 0.2126729, 0.0193339,
    0.3575761, 0.7151522, 0.1191920,
    0.1804375, 0.0721750, 0.9503041);

fn inSrgbGamut(iCh: vec3f) -> bool {
    let rgb = xyzToSrgb * iChToXyz(iCh);
    return all(rgb >= vec3f(0.0)) && all(rgb <= vec3f(1.0));
}

fn zcamTonemap(rgb: vec3f) -> vec3f {
    var iCh = xyzToICh(srgbToXyz * rgb);
    let s0 = 0.71;
    let s1 = 1.04;
    let p = 1.40;
    let t0 = 0.01;
    let n = s1 * pow(iCh.x / (iCh.x + s0), p);
    iCh.x = clamp(n * n / (n + t0), 0.0, 1.0);
    if (!inSrgbGamut(iCh)) {
        let c = iCh.y;
        iCh.y -= 0.5 * c;
        // GL's for (float i = 0.25; i >= 1/256; i *= 0.5): seven halvings.
        var stepSize = 0.25;
        for (var k = 0; k < 7; k++) {
            iCh.y += select(-stepSize, stepSize, inSrgbGamut(iCh)) * c;
            stepSize *= 0.5;
        }
    }
    return xyzToSrgb * iChToXyz(iCh);
}

// Uncharted 2 (Hable), as GL scales it.
fn uncharted2(xIn: vec3f) -> vec3f {
    let x = xIn * 0.28;
    let a = 0.28;
    let b = 0.29;
    let c = 0.10;
    let d = 0.2;
    let e = 0.025;
    let f = 0.35;
    return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;
}

fn unchartedTonemapping(color: vec3f) -> vec3f {
    return uncharted2(color * 4.7) / uncharted2(vec3f(15.2));
}

// Khronos PBR Neutral.
fn pbrNeutral(colorIn: vec3f) -> vec3f {
    let startCompression = 0.8 - 0.04;
    let desaturation = 0.15;
    var color = colorIn;
    let x = min(color.r, min(color.g, color.b));
    let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
    color -= offset;
    let peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) { return color; }
    let d = 1.0 - startCompression;
    let newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    let g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, newPeak * vec3f(1.0), g);
}

fn tonemap(c: vec3f) -> vec3f {
    switch post.mode.x {
        case 1: { return agx(c); }
        case 2: { return zcamTonemap(c); }
        case 3: { return unchartedTonemapping(c); }
        case 4: { return pbrNeutral(c); }
        default: { return acesFitted(c); }
    }
}

fn fromLinearSrgb(c: vec3f) -> vec3f {
    let low = c * 12.92;
    let high = 1.055 * pow(c, vec3f(1.0 / 2.4)) - 0.055;
    return select(high, low, c < vec3f(0.0031308));
}

// GL's toGammaSpace: AgX is already for display, the rest take the curve.
fn toDisplay(c: vec3f) -> vec3f {
    if (post.mode.x == 1) { return c; }
    return fromLinearSrgb(c);
}

// GL's adjustColor: grading before the tone mapper.
fn adjustColor(colorIn: vec3f) -> vec3f {
    var color = colorIn;
    let saturation = post.grade.y;
    let vibrance = post.grade.z;
    let gamma = post.grade.w;
    let luma = dot(color, vec3f(0.2126, 0.7152, 0.0722));
    color = mix(vec3f(luma), color, saturation);
    let satStrength = 1.0 - smoothstep(0.0, 1.0, max(color.r, max(color.g, color.b)));
    color = mix(color, mix(vec3f(luma), color, vibrance), satStrength);
    let shadowMask = smoothstep(0.0, 0.5, luma);
    let highlightMask = smoothstep(0.5, 1.0, luma);
    color *= mix(vec3f(1.0), vec3f(1.0 + post.boost.x), shadowMask);
    color *= mix(vec3f(1.0), vec3f(1.0 + post.boost.y), highlightMask);
    color = (color + post.lift.rgb) * post.gain.rgb;
    return pow(max(color, vec3f(0.0)), vec3f(gamma));
}

// Jorge Jimenez's gradient noise: the dither against banding.
fn gradientNoise(p: vec2f) -> f32 {
    return fract(52.9829189 * fract(dot(p, vec2f(0.06711056, 0.00583715))));
}

// GL's applyVignette. Its uv runs up from the bottom; the mask is the same
// either way (x(1-y) * y(1-x)).
fn applyVignette(color: vec3f, fragCoord: vec2f, intensity: f32) -> vec3f {
    var uv = fragCoord / vec2f(textureDimensions(source));
    uv *= 1.0 - uv.yx;
    var vig = uv.x * uv.y * 15.0;
    vig = pow(vig, mix(0.25, 1.0, intensity));
    return mix(color, color * vig, intensity);
}

// ---- Screen effects (milestone G2) -----------------------------------------

const lumaWeights = vec3f(0.2126, 0.7152, 0.0722);

// One read of the HDR color, with the double vision's ghost half over it.
fn ghostedTap(uv: vec2f, ghost: vec2f) -> vec3f {
    let color = textureSampleLevel(source, sourceSampler, uv, 0.0).rgb;
    if (all(ghost == vec2f(0.0))) { return color; }
    return mix(color, textureSampleLevel(source, sourceSampler, uv + ghost, 0.0).rgb, 0.5);
}

// The HDR color at uv, blurred, smeared toward the centre and doubled as the
// effects ask: 16 taps on a golden-angle disc, each also pulled toward the
// centre by its share of the radial blur.
fn sceneColor(uv: vec2f) -> vec3f {
    let size = vec2f(textureDimensions(source));
    let scale = size.y / 1080.0;
    let ghost = post.scene.zw * scale / size;
    let radius = post.scene.x * scale;
    let radial = post.scene.y;
    if (radius <= 0.0 && radial <= 0.0) { return ghostedTap(uv, ghost); }

    let toCentre = vec2f(0.5) - uv;
    var sum = ghostedTap(uv, ghost);
    for (var i = 0; i < 16; i++) {
        let f = (f32(i) + 0.5) / 16.0;
        let angle = f32(i) * 2.39996;
        let disc = vec2f(cos(angle), sin(angle)) * radius * sqrt(f) / size;
        sum += ghostedTap(uv + disc + toCentre * radial * 0.15 * f, ghost);
    }
    return sum / 17.0;
}

fn screenEffects(colorIn: vec3f, uv: vec2f) -> vec3f {
    var color = (colorIn - 0.5) * post.look.x + 0.5;
    let luma = dot(color, lumaWeights);
    let tintLuma = max(dot(post.tint.rgb, lumaWeights), 1e-3);
    color = mix(color, luma * post.tint.rgb / tintLuma, post.tint.a);
    color = mix(color, post.fade.rgb, post.fade.a);
    // 0 in the middle, 1 at the sides' middles, more in the corners.
    let edge = smoothstep(0.45, 1.25, length((uv - 0.5) * 2.0));
    return mix(color, post.edges.rgb, edge * post.edges.a);
}

@fragment
fn fs_toneMap(in: PostOutput) -> @location(0) vec4f {
    var color = sceneColor(in.uv) * post.grade.x;
    color = adjustColor(color);
    color = tonemap(color);
    color = toDisplay(color);
    color = screenEffects(color, in.uv);
    color += (1.0 / 255.0) * gradientNoise(in.position.xy) - (0.5 / 255.0);
    color = clamp(color, vec3f(0.0), vec3f(1.0));
    color = applyVignette(color, in.position.xy, post.boost.z);
    return vec4f(color, 1.0);
}

// ---- FXAA (fxaa.frag) -----------------------------------------------------

fn lumaSqr(color: vec3f) -> f32 {
    return sqrt(dot(color, vec3f(0.299, 0.587, 0.114)));
}

fn fetch(uv: vec2f) -> vec3f {
    return textureSampleLevel(source, sourceSampler, uv, 0.0).rgb;
}

// GL's quality(i): bigger steps the further the search goes.
fn quality(i: i32) -> f32 {
    if (i < 5) { return 1.0; }
    if (i >= 5 + 8) { return 8.0; }
    var r = array<f32, 8>(1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 6.0, 7.0);
    return r[i - 5];
}

const edgeMinThreshold = 0.028;
const edgeDarkThreshold = 0.125;
const iterations = 12;
const qualityMultiplier = 0.8;
const subpixelQuality = 0.95;

@fragment
fn fs_fxaa(in: PostOutput) -> @location(0) vec4f {
    let uv = in.uv;
    let colorCenter = fetch(uv);
    let lumaCenter = lumaSqr(colorCenter);

    // GL's "down" is ivec2(0, -1), toward its bottom row; this image is
    // top-first, so the same offset is one row up. The filter treats both
    // directions alike, so it only mirrors which side is called which.
    let lumaDown = lumaSqr(textureSampleLevel(source, sourceSampler, uv, 0.0, vec2i(0, -1)).rgb);
    let lumaUp = lumaSqr(textureSampleLevel(source, sourceSampler, uv, 0.0, vec2i(0, 1)).rgb);
    let lumaLeft = lumaSqr(textureSampleLevel(source, sourceSampler, uv, 0.0, vec2i(-1, 0)).rgb);
    let lumaRight = lumaSqr(textureSampleLevel(source, sourceSampler, uv, 0.0, vec2i(1, 0)).rgb);

    let lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    let lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
    let lumaRange = lumaMax - lumaMin;

    // Not an edge (or too dark to tell): the pixel as it is.
    if (lumaRange < max(edgeMinThreshold, lumaMax * edgeDarkThreshold)) {
        return vec4f(colorCenter, 1.0);
    }

    let lumaDownLeft = lumaSqr(textureSampleLevel(source, sourceSampler, uv, 0.0, vec2i(-1, -1)).rgb);
    let lumaUpRight = lumaSqr(textureSampleLevel(source, sourceSampler, uv, 0.0, vec2i(1, 1)).rgb);
    let lumaUpLeft = lumaSqr(textureSampleLevel(source, sourceSampler, uv, 0.0, vec2i(-1, 1)).rgb);
    let lumaDownRight = lumaSqr(textureSampleLevel(source, sourceSampler, uv, 0.0, vec2i(1, -1)).rgb);

    let lumaDownUp = lumaDown + lumaUp;
    let lumaLeftRight = lumaLeft + lumaRight;
    let lumaLeftCorners = lumaDownLeft + lumaUpLeft;
    let lumaDownCorners = lumaDownLeft + lumaDownRight;
    let lumaRightCorners = lumaDownRight + lumaUpRight;
    let lumaUpCorners = lumaUpRight + lumaUpLeft;

    let edgeHorizontal = abs(-2.0 * lumaLeft + lumaLeftCorners) + abs(-2.0 * lumaCenter + lumaDownUp) * 2.0
        + abs(-2.0 * lumaRight + lumaRightCorners);
    let edgeVertical = abs(-2.0 * lumaUp + lumaUpCorners) + abs(-2.0 * lumaCenter + lumaLeftRight) * 2.0
        + abs(-2.0 * lumaDown + lumaDownCorners);
    let isHorizontal = edgeHorizontal >= edgeVertical;

    let luma1 = select(lumaLeft, lumaDown, isHorizontal);
    let luma2 = select(lumaRight, lumaUp, isHorizontal);
    let gradient1 = luma1 - lumaCenter;
    let gradient2 = luma2 - lumaCenter;
    let is1Steepest = abs(gradient1) >= abs(gradient2);
    let gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

    let inverseScreenSize = 1.0 / vec2f(textureDimensions(source));
    var stepLength = select(inverseScreenSize.x, inverseScreenSize.y, isHorizontal);
    var lumaLocalAverage = 0.0;
    if (is1Steepest) {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
    } else {
        lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
    }

    // Half a pixel toward the edge, then search along it both ways.
    var currentUv = uv;
    if (isHorizontal) { currentUv.y += stepLength * 0.5; } else { currentUv.x += stepLength * 0.5; }
    let offset = select(vec2f(0.0, inverseScreenSize.y), vec2f(inverseScreenSize.x, 0.0), isHorizontal);
    var uv1 = currentUv - offset;
    var uv2 = currentUv + offset;

    var lumaEnd1 = lumaSqr(fetch(uv1)) - lumaLocalAverage;
    var lumaEnd2 = lumaSqr(fetch(uv2)) - lumaLocalAverage;
    var reached1 = abs(lumaEnd1) >= gradientScaled;
    var reached2 = abs(lumaEnd2) >= gradientScaled;
    var reachedBoth = reached1 && reached2;
    if (!reached1) { uv1 -= offset; }
    if (!reached2) { uv2 += offset; }

    if (!reachedBoth) {
        for (var i = 0; i < iterations; i++) {
            if (!reached1) { lumaEnd1 = lumaSqr(fetch(uv1)) - lumaLocalAverage; }
            if (!reached2) { lumaEnd2 = lumaSqr(fetch(uv2)) - lumaLocalAverage; }
            reached1 = abs(lumaEnd1) >= gradientScaled;
            reached2 = abs(lumaEnd2) >= gradientScaled;
            reachedBoth = reached1 && reached2;
            if (!reached1) { uv1 -= offset * quality(i) * qualityMultiplier; }
            if (!reached2) { uv2 += offset * quality(i) * qualityMultiplier; }
            if (reachedBoth) { break; }
        }
    }

    let distance1 = select(uv.y - uv1.y, uv.x - uv1.x, isHorizontal);
    let distance2 = select(uv2.y - uv.y, uv2.x - uv.x, isHorizontal);
    let isDirection1 = distance1 < distance2;
    let distanceFinal = min(distance1, distance2);
    let edgeThickness = distance1 + distance2;
    let pixelOffset = -distanceFinal / edgeThickness + 0.5;

    let isLumaCenterSmaller = lumaCenter < lumaLocalAverage;
    let correctVariation = (select(lumaEnd2, lumaEnd1, isDirection1) < 0.0) != isLumaCenterSmaller;
    var finalOffset = select(0.0, pixelOffset, correctVariation);

    // Sub-pixel aliasing: from the 3x3 average against the center.
    let lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight) + lumaLeftCorners + lumaRightCorners);
    let subPixelOffset1 = clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
    let subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
    let subPixelOffsetFinal = subPixelOffset2 * subPixelOffset2 * subpixelQuality;
    finalOffset = max(finalOffset, subPixelOffsetFinal);

    var finalUv = uv;
    if (isHorizontal) { finalUv.y += finalOffset * stepLength; } else { finalUv.x += finalOffset * stepLength; }
    return vec4f(fetch(finalUv), 1.0);
}
