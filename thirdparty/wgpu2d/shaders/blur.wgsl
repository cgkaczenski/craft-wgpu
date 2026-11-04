// One separable gaussian pass, and the first thing in this project to use the
// capability that makes compute different from a fragment shader.
//
// A fragment shader could blur. What it could not do is *share work between
// neighbouring pixels*. With a radius of 8, every output reads 17 inputs, and
// the output next door reads 16 of the same 17. A fragment shader has no way
// to say so: its invocations are independent by construction, so it pays for
// all 17 every time.
//
// A compute shader's invocations arrive in workgroups, and a workgroup has
// memory of its own. This one loads its span of the line once into
// `tile` -- 64 pixels plus a margin of 8 at each end -- waits at a
// `workgroupBarrier` until every invocation has finished loading, and then
// reads its 17 neighbours out of that instead of out of the texture. 80 loads
// serve 64 outputs rather than 1088.
//
// The barrier is the whole point and is not optional. Without it an invocation
// can read a slot its neighbour has not written yet, and the result is the
// kind of bug that looks like noise and changes with the weather.
//
// Separable, because a 17x17 gaussian is the product of two 17-taps: 289
// samples becomes 34. That saving is available to a fragment shader too, which
// is why it is the *shared memory*, not the separability, that this is here to
// demonstrate.

const RADIUS: i32 = 8;
const SPAN: i32 = 64;

@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var dst: texture_storage_2d<rgba8unorm, write>;

struct Params {
    // xy = the axis to blur along, (1,0) or (0,1). z = sigma. w unused.
    a: vec4f,
};
@group(0) @binding(2) var<uniform> params: Params;

// SPAN outputs need SPAN + 2*RADIUS inputs: the margin is what the invocations
// at each end of the workgroup reach into.
var<workgroup> tile: array<vec3f, 80>;

@compute @workgroup_size(64, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3u,
           @builtin(local_invocation_id) lid: vec3u)
{
    let size = vec2i(textureDimensions(dst));
    let axis = vec2i(i32(round(params.a.x)), i32(round(params.a.y)));
    let perp = vec2i(axis.y, axis.x);
    let sigma = max(params.a.z, 0.1);

    // Where this invocation sits: `along` runs down the blurred axis, `line`
    // picks which row or column. One shader for both passes, because the only
    // difference between them is which way this vector points.
    let along = i32(gid.x);
    let line = i32(gid.y);
    let base = i32(gid.x) - i32(lid.x); // first output of this workgroup

    // The cooperative load. Each invocation fetches its own pixel and, for the
    // first 2*RADIUS of them, one of the margin pixels as well. Clamped at the
    // ends of the line, which is what makes the edge of the picture bleed
    // outward rather than wrap.
    let limit = axis.x * size.x + axis.y * size.y;
    for (var slot = i32(lid.x); slot < SPAN + 2 * RADIUS; slot = slot + SPAN) {
        let sampleAlong = clamp(base + slot - RADIUS, 0, limit - 1);
        let coord = axis * sampleAlong + perp * line;
        tile[slot] = textureLoad(src, vec2u(coord), 0).rgb;
    }

    // Nothing below may run until every load above has landed.
    workgroupBarrier();

    if (along >= limit || line >= (perp.x * size.x + perp.y * size.y)) { return; }

    var sum = vec3f(0.0);
    var weightSum = 0.0;
    for (var i = -RADIUS; i <= RADIUS; i = i + 1) {
        let t = f32(i) / sigma;
        let w = exp(-0.5 * t * t);
        sum = sum + tile[i32(lid.x) + RADIUS + i] * w;
        weightSum = weightSum + w;
    }

    let coord = axis * along + perp * line;
    textureStore(dst, vec2u(coord), vec4f(sum / weightSum, 1.0));
}
