// The inventory's block icons (Renderer::renderAllBlocksUiTextures,
// renderBlockUi.vert/.frag): each block drawn once, at startup, into a 64x64
// target that the UI then draws as a sprite.
//
// The pipeline layout is the dropped-cube one (wgpuWorld.cpp): group 0 is the
// world's bind group, of which only the block textures are used; group 1 is
// the entity geometry and, in place of the cube records, one record per icon.
// Each icon is one draw, with firstInstance naming its record.

@group(0) @binding(3) var<storage, read> textureLayers: array<u32>;
@group(0) @binding(4) var blockTextures: texture_2d_array<f32>;
@group(0) @binding(5) var blockSampler: sampler;

struct Icon {
    // GL's u_viewProjection, with depth already remapped to 0..1.
    viewProjection: mat4x4f,
    // Texture slots (BlocksLoader::gpuIds indices) of the six faces.
    slotsA: vec4u,
    slotsB: vec4u,
    // x: GL's u_useOneTexture (every face takes slotsA.x).
    flags: vec4u,
};

// Renderer::create's entity geometry: the cube's 36 vertices, then each block
// model's (stairs, slabs, walls, furniture); 8 floats each: position, normal,
// uv.
@group(1) @binding(0) var<storage, read> vertices: array<f32>;
@group(1) @binding(1) var<storage, read> icons: array<Icon>;

struct IconOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) @interpolate(flat) layer: u32,
    @location(2) @interpolate(flat) normal: vec3f,
};

@vertex
fn vs_icon(@builtin(vertex_index) vertexIndex: u32, @builtin(instance_index) instance: u32) -> IconOutput {
    let icon = icons[instance];
    let base = vertexIndex * 8u;
    let shape = vec3f(vertices[base], vertices[base + 1u], vertices[base + 2u]);
    let normal = vec3f(vertices[base + 3u], vertices[base + 4u], vertices[base + 5u]);

    // GL's v_id / 6: gl_VertexID counts from the draw's first vertex, as
    // vertex_index does, so this is the face only for the cube (first 0).
    // The models use one texture.
    let face = vertexIndex / 6u;
    var slot = icon.slotsA.x;
    if (icon.flags.x == 0u) {
        slot = icon.slotsA[min(face, 3u)];
        if (face >= 4u) { slot = icon.slotsB[min(face - 4u, 1u)]; }
    }

    var out: IconOutput;
    out.position = icon.viewProjection * vec4f(shape, 1.0);
    out.uv = vec2f(vertices[base + 6u], vertices[base + 7u]);
    out.layer = textureLayers[slot];
    out.normal = normal;
    return out;
}

@fragment
fn fs_icon(in: IconOutput) -> @location(0) vec4f {
    let color = textureSample(blockTextures, blockSampler, in.uv, in.layer);
    if (color.a <= 0.0) { discard; }
    let light = 0.65 + 0.35 * clamp(dot(in.normal, normalize(vec3f(-0.5, 0.8, 0.2))), 0.0, 1.0);
    return vec4f(color.rgb * light, color.a);
}
