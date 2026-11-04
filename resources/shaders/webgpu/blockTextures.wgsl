// Milestone 7: building the block texture array, on the GPU.
//
// OpenGL gave every block texture its own bindless handle. WebGPU has no
// bindless, and a binding array is capped at maxSampledTexturesPerShaderStage
// (128 here), so the textures go into one texture_2d_array instead: 128x128,
// one layer per distinct texture, 5 mip levels (GL clamps at LOD 4).
//
// Two entry points, both one invocation per destination texel:
//
//   fill: one source texture (a wgpu2d sprite texture, any size) into one
//         layer of level 0. A smaller source is enlarged by nearest texel, so
//         16x16 and 64x64 pixel art stays exact; a larger one (the tall
//         128x256..128x1280 strips, sampled whole by the GL shader) is box
//         averaged. Rows are flipped: wgpu2d stores images top row first,
//         and the array stores them bottom row first like the GL upload, so
//         the game's UV tables carry over unchanged (docs/roadmap.md,
//         "Conventions").
//
//   downsample: level N from level N-1, every layer at once (z is the layer):
//         the rounded 2x2 average of the bytes, as wgpu2d's mipmap.wgsl does.

@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var destination: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(8, 8)
fn fill(@builtin(global_invocation_id) id: vec3u)
{
    let dstSize = textureDimensions(destination);
    if (id.x >= dstSize.x || id.y >= dstSize.y) { return; }

    let srcSize = textureDimensions(source);

    // The source rectangle this destination texel covers, at least one texel
    // each way. Destination row 0 is the image's bottom, source row 0 its top.
    let flippedY = dstSize.y - 1u - id.y;
    let x0 = (id.x * srcSize.x) / dstSize.x;
    let y0 = (flippedY * srcSize.y) / dstSize.y;
    let x1 = max(((id.x + 1u) * srcSize.x) / dstSize.x, x0 + 1u);
    let y1 = max(((flippedY + 1u) * srcSize.y) / dstSize.y, y0 + 1u);

    var sum = vec4f(0.0);
    for (var y = y0; y < y1; y++)
    {
        for (var x = x0; x < x1; x++)
        {
            sum += round(textureLoad(source, vec2u(x, y), 0) * 255.0);
        }
    }
    let count = f32((x1 - x0) * (y1 - y0));
    textureStore(destination, id.xy, floor(sum / count + 0.5) / 255.0);
}

// Bindings 2 and 3, not 0 and 1: a module may not declare two resources at
// the same binding, even for different entry points. Each pipeline gets an
// automatic layout that holds only the bindings its entry point uses.
@group(0) @binding(2) var upperLevel: texture_2d_array<f32>;
@group(0) @binding(3) var lowerLevel: texture_storage_2d_array<rgba8unorm, write>;

@compute @workgroup_size(8, 8)
fn downsample(@builtin(global_invocation_id) id: vec3u)
{
    let dstSize = textureDimensions(lowerLevel);
    if (id.x >= dstSize.x || id.y >= dstSize.y) { return; }

    let layer = id.z;
    let p = 2u * id.xy;
    let a = round(textureLoad(upperLevel, p, layer, 0) * 255.0);
    let b = round(textureLoad(upperLevel, p + vec2u(1u, 0u), layer, 0) * 255.0);
    let c = round(textureLoad(upperLevel, p + vec2u(0u, 1u), layer, 0) * 255.0);
    let d = round(textureLoad(upperLevel, p + vec2u(1u, 1u), layer, 0) * 255.0);
    textureStore(lowerLevel, id.xy, layer, floor((a + b + c + d + 2.0) * 0.25) / 255.0);
}
