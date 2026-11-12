// The light of what the player holds (a torch): the part the world's shaders
// (lighting.wgsl, through world.wgsl) and the rain's (precipitation.wgsl)
// share. Nothing here reads a uniform; each caller hands in its own copy of
// the settings:
//
//   held.x  the light level at the player, 0-15 (0: nothing held gives light)
//   held.y  how far it reaches, in blocks
//   held.z  how many bands it falls off in
//   held.w  how much of a band's width is dithered into the next, 0..1
//
// The light is a block light that walks with the player: a level by distance
// alone, as a torch block's is, in a few wide steps where a torch block's
// takes one a block. A hard step from band to band is a ring; so each band's
// edge is dithered into the next, more of the brighter one the further in.

// Where the light is, from the camera: about the chest, so the ground at the
// player's feet is in the first band.
const heldLightOrigin = vec3f(0.0, -0.4, 0.0);

// A 4x4 Bayer matrix's threshold for a cell: 16 values evenly spread over
// 0..1, laid out so that neighbours differ as much as they can.
fn bayer4(cell: vec2i) -> f32 {
    var m = array<f32, 16>(
         0.0,  8.0,  2.0, 10.0,
        12.0,  4.0, 14.0,  6.0,
         3.0, 11.0,  1.0,  9.0,
        15.0,  7.0, 13.0,  5.0);
    return (m[u32(cell.y & 3) * 4u + u32(cell.x & 3)] + 0.5) / 16.0;
}

// The band a point `distance` blocks from the light is in: 0 out of reach, up
// to held.z at the player. `threshold` (0..1, different from point to point)
// decides which side of a band's edge a point near it falls: quantising with
// a dither, kept to the edges so the bands stay flat between them.
fn heldLightBand(distance: f32, held: vec4f, threshold: f32) -> f32 {
    if (held.x <= 0.0 || held.y <= 0.0 || held.z <= 0.0) { return 0.0; }
    // 0 at the light's reach, held.z at the player; whole numbers are the
    // bands' edges.
    let x = (1.0 - distance / held.y) * held.z;
    if (x <= -0.5) { return 0.0; }
    // The nearest edge, and how far past it (-0.5..0.5) the point is.
    let edge = floor(x + 0.5);
    let past = x - edge;
    // The chance of the band inside the edge: 0 to 1 across the dithered
    // strip, which is centred on the edge.
    let inner = clamp(past / max(held.w, 0.001) + 0.5, 0.0, 1.0);
    return clamp(edge + select(0.0, 1.0, inner > threshold), 0.0, held.z);
}

// A band's light level, 0-15.
fn heldLightLevelOfBand(band: f32, held: vec4f) -> f32 {
    return round(held.x * band / max(held.z, 1.0));
}
