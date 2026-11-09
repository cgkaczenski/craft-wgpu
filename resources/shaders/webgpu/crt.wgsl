// A CRT filter over the finished frame: curvature, scanlines, an aperture
// mask, colour fringing and a vignette. The phosphor glow is wgpu2d's
// FinalGlow, already added to the frame before this runs (rendering/crt.cpp
// sets both).
//
// A fragment stage only; the vertex stage is wgpu2d's sprite shader.

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) color: vec4f,
    @location(1) uv: vec2f,
};

@group(0) @binding(0) var spriteTexture: texture_2d<f32>;
@group(0) @binding(1) var spriteSampler: sampler;

struct EffectUniforms {
    resolution: vec4f, // xy = pixels, zw = 1 / pixels
    time: vec4f,       // x = seconds
    a: vec4f,          // x = master, y = curvature, z = scanlines, w = mask
    b: vec4f,          // x = scanline period in pixels, y = vignette, z = fringing, w = warmth
};
@group(2) @binding(0) var<uniform> effect: EffectUniforms;

// Where warm highlights head. Kept in step with `warmColour` in crt.cpp, which
// tints the glow toward the same colour.
const warmTint = vec3f(1.0, 0.80, 0.58);
const lumaWeights = vec3f(0.2126, 0.7152, 0.0722);

// Where the highlight shoulder begins: below it colour is untouched, above it
// values ease toward 1, so the brightness restore does not clip highlights.
const shoulderStart = 0.9;

fn shoulder(c: vec3f) -> vec3f
{
    let k = shoulderStart;
    let over = max(c - vec3f(k), vec3f(0.0));
    return min(c, vec3f(k)) + (1.0 - k) * (vec3f(1.0) - exp(-over / (1.0 - k)));
}

// The glass: a barrel distortion, which leaves the centre and moves the
// corners most. toPicture in crt.cpp is this on the CPU, for the mouse.
fn bend(uv: vec2f, amount: f32) -> vec2f
{
    let centred = uv * 2.0 - 1.0;
    let r2 = dot(centred, centred);
    return (centred * (1.0 + amount * r2)) * 0.5 + 0.5;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    // One master knob scales the lot, so the parts stay in proportion.
    let master = clamp(effect.a.x, 0.0, 2.0);

    let curvature = effect.a.y * master;
    let scanlines = effect.a.z * master;
    let mask = effect.a.w * master;
    let period = effect.b.x;
    let vignette = effect.b.y * master;
    let fringing = effect.b.z * master;
    let warmth = clamp(effect.b.w, 0.0, 1.0);

    let uv = bend(in.uv, curvature);

    // Fringing: the three guns do not land in quite the same place, so red
    // and blue are sampled through slightly different curvatures.
    var colour = textureSampleLevel(spriteTexture, spriteSampler, uv, 0.0).rgb;
    if (fringing > 0.0) {
        let scale = fringing * 0.004;
        let red = textureSampleLevel(spriteTexture, spriteSampler,
            bend(in.uv, curvature + scale), 0.0).r;
        let blue = textureSampleLevel(spriteTexture, spriteSampler,
            bend(in.uv, curvature - scale), 0.0).b;
        colour = vec3f(red, colour.g, blue);
    }

    // Warm highlights: only the bright end moves, toward amber at the same
    // luminance.
    if (warmth > 0.0) {
        let luma = dot(colour, lumaWeights);
        let highlight = smoothstep(0.45, 0.95, luma);
        let warmed = luma * warmTint / dot(warmTint, lumaWeights);
        colour = mix(colour, warmed, warmth * highlight);
    }

    // Outside the tube, black rather than the clamped edge texel.
    let inside = f32(uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0);

    // Every darkening term below records what it took, so `gain` can give it
    // back: a tube does not get dimmer as its mask gets finer.
    var gain = 1.0;

    // Scanlines, in whole output pixels; a count of lines across the picture
    // would give a fractional period that drifts against the pixel grid.
    if (scanlines > 0.0) {
        let row = in.uv.y * effect.resolution.y;
        let phase = fract(row / max(period, 2.0));
        let line = 0.5 - 0.5 * cos(phase * 6.2831853);
        colour = colour * (1.0 - scanlines * (1.0 - line));
        gain = gain / (1.0 - scanlines * 0.5); // `line` averages 0.5
    }

    // The aperture grille: stripes of red, green and blue phosphor, per output
    // pixel because it belongs to the screen rather than the picture.
    if (mask > 0.0) {
        let column = i32(floor(in.uv.x * effect.resolution.x)) % 3;
        var tint = vec3f(1.0, 1.0, 1.0);
        if (column == 0) { tint = vec3f(1.0, 1.0 - mask, 1.0 - mask); }
        else if (column == 1) { tint = vec3f(1.0 - mask, 1.0, 1.0 - mask); }
        else { tint = vec3f(1.0 - mask, 1.0 - mask, 1.0); }
        colour = colour * tint;
        gain = gain / (1.0 - mask * 2.0 / 3.0); // full on one column in three
    }

    // Capped, so a slider at its limit brightens rather than blows out.
    colour = shoulder(colour * min(gain, 2.0));

    // The corners of the tube are further from the gun and dimmer for it.
    if (vignette > 0.0) {
        let centred = uv * 2.0 - 1.0;
        colour = colour * (1.0 - vignette * dot(centred, centred) * 0.5);
    }

    return vec4f(colour * inside, 1.0);
}
