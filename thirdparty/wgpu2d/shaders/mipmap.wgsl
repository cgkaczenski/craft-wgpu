// Mip level generation, on the GPU, one output texel per invocation.
//
// This is the library's own shader, embedded at build time like quad.wgsl.
//
// It is a *compute* shader, and the reason is worth stating. A fragment shader
// could do this too -- draw a full-screen quad into level N+1 sampling level N
// -- but that would mean a render pass, a pipeline whose colour target format
// has to match, a viewport, and a rasteriser turning triangles into the very
// per-texel invocations we already know we want. The work here is "run this
// function once per output texel"; a compute pass says exactly that and
// nothing else. Note especially the *output*: a fragment shader writes to the
// one pixel the rasteriser made it for, whereas `textureStore` writes to a
// coordinate this shader chooses.

// Read: an ordinary sampled texture binding, but loaded rather than sampled --
// no sampler, no filtering, exact texels. A mip reduction wants precisely the
// four texels below, not whatever a sampler would blend.
@group(0) @binding(0) var srcLevel: texture_2d<f32>;

// Write: a storage texture. The format is spelled out in the shader because it
// is part of the binding's type, not a property of whatever is bound -- which
// is why the bind group layout has to repeat it, and why they must agree.
@group(0) @binding(1) var dstLevel: texture_storage_2d<rgba8unorm, write>;

// 64 invocations per workgroup, the size the guides land on for image work.
// The dispatch is in *workgroups*, not invocations, so the caller rounds up
// and the bounds check below discards the overhang. A 3x3 destination still
// launches one whole workgroup of 64.
@compute @workgroup_size(8, 8)
fn cs_main(@builtin(global_invocation_id) id: vec3u)
{
    let dstSize = textureDimensions(dstLevel);
    if (id.x >= dstSize.x || id.y >= dstSize.y) { return; }

    // The 2x2 block below, clamped so an odd-sized level reads its last row
    // and column twice instead of off the edge. Same rule as the CPU version
    // this replaces.
    let srcSize = textureDimensions(srcLevel);
    let x0 = min(2u * id.x, srcSize.x - 1u);
    let x1 = min(2u * id.x + 1u, srcSize.x - 1u);
    let y0 = min(2u * id.y, srcSize.y - 1u);
    let y1 = min(2u * id.y + 1u, srcSize.y - 1u);

    // Back to the integers the bytes actually are. An rgba8unorm texel arrives
    // as n/255 in a float, and rounding after multiplying by 255 recovers n
    // exactly -- sums up to 1020 are exact in f32, far below the 2^24 where
    // that stops being true. Doing it this way means `(sum + 2) / 4` is the
    // same round-half-up the CPU did, so the two paths can be compared texel
    // for texel instead of "close enough".
    let a = round(textureLoad(srcLevel, vec2u(x0, y0), 0) * 255.0);
    let b = round(textureLoad(srcLevel, vec2u(x1, y0), 0) * 255.0);
    let c = round(textureLoad(srcLevel, vec2u(x0, y1), 0) * 255.0);
    let d = round(textureLoad(srcLevel, vec2u(x1, y1), 0) * 255.0);

    let averaged = floor((a + b + c + d + 2.0) * 0.25);
    textureStore(dstLevel, id.xy, averaged / 255.0);
}
