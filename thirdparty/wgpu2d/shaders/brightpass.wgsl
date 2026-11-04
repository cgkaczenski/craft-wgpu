// The first half of a bloom: keep only what is bright enough to glow, at half
// resolution.
//
// Two jobs in one dispatch, and both are deliberate.
//
// The *threshold* is what stops a bloom from being a haze. Blurring the whole
// picture and adding it back lifts the black level everywhere; blurring only
// what is already bright adds light where a real phosphor would spill it.
//
// The *halving* is free quality. A bloom is low-frequency by definition, so
// full resolution buys nothing, and working at half costs a quarter of the
// samples while doubling the reach of a blur of the same radius in texels.
// This is the same 2x2 average the mip generator does, for the same reason.

@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var dst: texture_storage_2d<rgba8unorm, write>;

struct Params {
    // x = threshold, y = knee (how soft the cut is), zw unused
    a: vec4f,
};
@group(0) @binding(2) var<uniform> params: Params;

@compute @workgroup_size(8, 8)
fn cs_main(@builtin(global_invocation_id) id: vec3u)
{
    let dstSize = textureDimensions(dst);
    if (id.x >= dstSize.x || id.y >= dstSize.y) { return; }

    let srcSize = textureDimensions(src);
    let p = id.xy * 2u;
    let x1 = min(p.x + 1u, srcSize.x - 1u);
    let y1 = min(p.y + 1u, srcSize.y - 1u);

    let colour = (textureLoad(src, vec2u(p.x, p.y), 0).rgb
        + textureLoad(src, vec2u(x1, p.y), 0).rgb
        + textureLoad(src, vec2u(p.x, y1), 0).rgb
        + textureLoad(src, vec2u(x1, y1), 0).rgb) * 0.25;

    // Perceived brightness, not the average of the channels: a saturated blue
    // and a saturated yellow of the same numeric average do not read as
    // equally bright, and a bloom that ignores that glows in the wrong places.
    let luma = dot(colour, vec3f(0.2126, 0.7152, 0.0722));

    // A soft knee rather than a hard cut. A hard threshold makes the bloom
    // pop in and out as a sprite's brightness crosses it, which flickers on
    // anything animated -- and everything here is animated.
    let threshold = params.a.x;
    let knee = max(params.a.y, 0.0001);
    let weight = clamp((luma - threshold) / knee, 0.0, 1.0);

    textureStore(dst, id.xy, vec4f(colour * weight, 1.0));
}
