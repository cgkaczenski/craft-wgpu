// The snow that lies on the ground (snowCover.cpp; wgpuWorld's recordSnow):
// a mesh round the camera, lifted off the tops of the blocks by how deep the
// snow is on each column and pressed down along the tracks.
//
// The mesh is a grid, `cells` to a block, its vertices in the middles of the
// cells (so none lies on the line between two blocks): a strip per row, from
// the vertex and instance indices alone. A vertex stands on its column's top
// face, as high as the snow is deep there. Toward a drop (a lower column, or
// one without snow) the snow thins to nothing in a round edge, so it lies on
// every ledge as a pillow; against a wall it keeps its depth. The triangles
// between two columns of different heights are not drawn.
//
// The tracks are a picture of the snow from above (16 texels to a block,
// wrapping every 64 blocks): 128/255 is untouched snow, less is pressed
// down, more is piled up. The vertices take their height from it; the
// fragments take the slope, so a footprint is shaded finer than the mesh.
//
// Lit as the entities are (entities.wgsl's lightEntity), into the G-buffer.

#include "world.wgsl"
#include "lighting.wgsl"

struct SnowUniforms {
    // xy column (0, 0)'s world block x and z, z columns per side, w the
    // mesh's cells to a block.
    corner: vec4i,
    // x how far from the camera the snow has thinned to nothing, in blocks;
    // y the blocks the tracks' picture spans; z its texels; w unused.
    params: vec4f,
};
@group(1) @binding(0) var<uniform> snow: SnowUniforms;
// Per column, x fastest: x the world y of the top face, y the snow's depth
// (0 none), z the light above it, as baked.
@group(1) @binding(1) var<storage, read> columns: array<vec4f>;
@group(1) @binding(2) var trail: texture_2d<f32>;
@group(1) @binding(3) var trailSampler: sampler;

struct GBuffer {
    @location(0) color: vec4f,
    @location(1) position: vec4f, // w: roughness
    @location(2) normal: vec4u,
    @location(3) bloom: vec4f,
};

struct SnowOutput {
    @builtin(position) position: vec4f,
    @location(0) relative: vec3f,
    // The depth here with the round edges, before the tracks; and how it
    // changes along x and z.
    @location(1) base: f32,
    @location(2) slope: vec2f,
    @location(3) trailUV: vec2f,
    // The column's top: the same at a triangle's three corners unless it
    // spans two columns of different heights.
    @location(4) top: f32,
    @location(5) @interpolate(flat) topFlat: f32,
    @location(6) @interpolate(flat) light: u32,
};

fn column(c: vec2i) -> vec4f {
    return columns[u32(c.x + c.y * snow.corner.z)];
}

// A track's value to how much of the depth is left: packed at the bottom
// (snowCover.cpp's `packed`), whole at untouched, half as much again piled.
fn trailHeight(t: f32) -> f32 {
    return mix(0.12, 1.0, min(t * 2.0, 1.0)) + 0.9 * max(t * 2.0 - 1.004, 0.0);
}

// From column `own`, is `other` beside it a drop: lower, or level and bare.
fn isDrop(own: vec4f, other: vec4f) -> bool {
    return other.x < own.x - 0.01 || (other.y <= 0.0 && other.x < own.x + 0.01);
}

// How deep the snow is at `l` (-0.5..0.5 from the block's middle) on column
// `c`, round edges in.
fn snowBase(c: vec2i, l: vec2f) -> f32 {
    let own = column(c);
    if (own.y <= 0.0) { return 0.0; }
    let side = vec2i(select(vec2f(-1.0), vec2f(1.0), l >= vec2f(0.0)));
    let w = abs(l);
    let nx = column(c + vec2i(side.x, 0));
    let nz = column(c + vec2i(0, side.y));
    let nd = column(c + side);

    // Level neighbours' depths blend in toward the edges, so the snow has
    // no step where one column has more than the next.
    let dx = select(own.y, nx.y, abs(nx.x - own.x) < 0.01);
    let dz = select(own.y, nz.y, abs(nz.x - own.x) < 0.01);
    let dd = select(own.y, nd.y, abs(nd.x - own.x) < 0.01);
    let depth = mix(mix(own.y, dx, w.x), mix(dz, dd, w.x), w.y);

    // The round edge: a quarter circle over the last third of the block.
    let reach = 0.36;
    let toEdge = vec2f(0.5) - w;
    var e = 1.0;
    if (isDrop(own, nx)) { e = min(e, toEdge.x / reach); }
    if (isDrop(own, nz)) { e = min(e, toEdge.y / reach); }
    if (isDrop(own, nd)) { e = min(e, length(toEdge) / reach); }
    let inward = 1.0 - e;
    return depth * sqrt(max(1.0 - inward * inward, 0.0));
}

@vertex
fn vs_snow(@builtin(vertex_index) vertexIndex: u32, @builtin(instance_index) row: u32) -> SnowOutput {
    let cells = u32(snow.corner.w);
    let cell = vec2u(vertexIndex >> 1u, row + (vertexIndex & 1u));
    // The map has a column all round the mesh's.
    let c = vec2i(cell / cells) + vec2i(1);
    let l = (vec2f(cell % cells) + 0.5) / f32(cells) - 0.5;
    let own = column(c);
    let block = snow.corner.xy + c;

    let across = vec2f(block - u.positionInt.xz) - u.positionFloat.xz + l;
    // Thinner far away, down to the bare (and already white) tops.
    let fade = 1.0 - smoothstep(snow.params.x - 9.0, snow.params.x, length(across));

    // The slope, from a step toward the block's middle (so it stays on
    // this column).
    let nudge = -select(vec2f(-1.0), vec2f(1.0), l >= vec2f(0.0)) / 32.0;
    let base = snowBase(c, l);
    let slope = vec2f(snowBase(c, l + vec2f(nudge.x, 0.0)) - base, snowBase(c, l + vec2f(0.0, nudge.y)) - base) / nudge;

    // Where in the tracks' picture: the corner's place in it, and on from
    // there (it wraps, so the sampler repeats).
    let span = i32(snow.params.y);
    let cornerIn = ((snow.corner.xy % span) + span) % span;
    let trailUV = (vec2f(cornerIn + c) + l + 0.5) / snow.params.y;
    let t = textureSampleLevel(trail, trailSampler, trailUV, 0.0).r;

    let height = base * fade * trailHeight(t);
    let relative = vec3f(across.x, (own.x - f32(u.positionInt.y)) - u.positionFloat.y + height, across.y);

    var out: SnowOutput;
    out.position = psxSnap(u.viewProjection * vec4f(relative, 1.0));
    out.relative = relative;
    out.base = base * fade;
    out.slope = slope * fade;
    out.trailUV = trailUV;
    out.top = own.x;
    out.topFlat = own.x;
    out.light = u32(own.z);
    return out;
}

fn hash2(p: vec2f) -> f32 {
    return fract(sin(dot(p, vec2f(12.9898, 78.233))) * 43758.5453);
}

@fragment
fn fs_snow(in: SnowOutput) -> GBuffer {
    // The tracks here, and their slope from a texel either side.
    let texel = 1.0 / snow.params.z;
    let here = trailHeight(textureSample(trail, trailSampler, in.trailUV).r);
    let east = trailHeight(textureSample(trail, trailSampler, in.trailUV + vec2f(texel, 0.0)).r);
    let west = trailHeight(textureSample(trail, trailSampler, in.trailUV - vec2f(texel, 0.0)).r);
    let south = trailHeight(textureSample(trail, trailSampler, in.trailUV + vec2f(0.0, texel)).r);
    let north = trailHeight(textureSample(trail, trailSampler, in.trailUV - vec2f(0.0, texel)).r);

    // Not between two columns of different heights, and not where the snow
    // has thinned to nothing.
    if (abs(in.top - in.topFlat) > 0.004 || in.base * here < 0.008) { discard; }

    // Blocks of height per block along x and z.
    let blocksPerTexel = snow.params.y / snow.params.z;
    let trailSlope = vec2f(east - west, south - north) / (2.0 * blocksPerTexel);
    let slope = here * in.slope + in.base * trailSlope;
    let N = normalize(vec3f(-slope.x, 1.0, -slope.y));

    // White, a speckle to each of the blocks' texels (16 to a block, as the
    // snow weather's cover on the faces has); pressed snow is denser, so
    // darker and bluer.
    let cell = floor(fract(in.trailUV) * snow.params.z);
    let speckle = hash2(cell);
    let pressed = clamp(1.0 - here, 0.0, 1.0);
    var albedo = vec3f(0.74, 0.77, 0.82) * (0.94 + 0.1 * speckle);
    albedo = mix(albedo, vec3f(0.44, 0.51, 0.66), pressed * 0.7);

    // The light, as on an entity: the sun or moon with its shadow, the
    // ambient, the light blocks. Rough, not metal.
    let levels = lightLevels(in.light);
    let viewDepth = -(u.view * vec4f(in.relative, 1.0)).z;
    let shadow = sunShadow(in.relative, viewDepth, dot(u.sunDirection.xyz, N), in.position.xy);
    let ambient = computeAmbient(withHeldLight(levels, in.relative, N), N, 1.0, false, 1.0, shadow);
    let roughness = 0.85;
    let F0 = vec3f(0.04);
    let V = -normalize(in.relative);
    let world = u.positionFloat.xyz + in.relative;
    let worldCell = floor(world + vec3f(0.5));
    let blockPos = u.positionInt.xyz + vec3i(worldCell);
    var direct = lobesSum(
        pointLights(levels, blockPos, world - worldCell, 0.0, roughness, V, albedo, N, F0, vec3f(1.0)),
        sunLight(levels, 0.0, roughness, V, albedo, N, F0, 1.0, shadow, vec3f(1.0)));

    // Crystals that catch the sun: a few texels (finer than the speckle)
    // flash, different ones as the eye moves.
    let grain = floor(fract(in.trailUV) * snow.params.z * 2.0);
    let turn = floor(dot(V, vec3f(17.0, 9.0, 13.0)));
    let glint = step(0.994, hash2(grain + vec2f(turn * 7.0, turn * 3.0)));
    // (Mirrored sunlight, so it counts with the specular lobe.)
    direct.specular += u.sunLightColor.rgb
        * (glint * shadow * max(dot(N, u.sunDirection.xyz), 0.0) * 0.5 * lowSunStrength());
    let color = lightShown(direct, albedo * ambient);

    var out: GBuffer;
    out.color = vec4f(applyWeatherFog(color, in.relative, N, levels.skyUnchanged), 1.0);
    out.position = vec4f((u.view * vec4f(in.relative, 1.0)).xyz, 0.9);
    out.normal = packNormal(N);
    out.bloom = vec4f(0.0);
    return out;
}
