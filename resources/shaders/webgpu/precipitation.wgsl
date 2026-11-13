// Milestone G1b: rain and snow (wgpuWorld's recordPrecipitation).
//
// One sheet per block column around the camera, turned toward it about the
// vertical, from where the column's rain stops (the top of its highest
// blocking block, so roofs, trees and cave ceilings keep it out) up to a
// little above the camera. The drops are drawn on the sheet, not modelled:
// a few lanes across each sheet, each lane its own speed and spacing from a
// hash of the column, scrolling down with time. Snow is the same with slow,
// swaying round flakes. Drawn after the transparent faces onto the HDR
// color, blended, depth tested against the scene and not written.

struct PrecipitationUniforms {
    // perspectiveRH_ZO times Camera::getViewMatrix (rotation only), as the
    // world's: camera-relative positions to clip space.
    viewProjection: mat4x4f,
    // xz column (0, 0)'s block relative to the camera; y the camera's
    // height (world y); w how far above and below the camera sheets reach.
    origin: vec4f,
    // xy the world block x and z of column (0, 0); z columns per side.
    corner: vec4i,
    // x the intensity (0..1), y seconds, z the brightness of the light the
    // drops catch, w the wind's lean (blocks sideways per block down).
    params: vec4f,
    // The light of what the player holds, as world.wgsl's u.heldLight.
    held: vec4f,
};
@group(0) @binding(0) var<uniform> p: PrecipitationUniforms;
// Per column: x the world y the rain stops at, y 0 none, 1 rain, 2 snow.
@group(0) @binding(1) var<storage, read> columns: array<vec2f>;

struct PrecipitationOutput {
    @builtin(position) position: vec4f,
    @location(0) sheet: vec2f,                         // x across 0..1, y world height
    @location(1) @interpolate(flat) column: vec2f,     // world x, z (for the hash)
    @location(2) @interpolate(flat) kindAndFade: vec2f, // x kind, y fade with distance
    @location(3) relative: vec3f,                      // from the camera, for the held light
};

#include "heldLight.wgsl"

// What the held light adds to a drop or a flake: rgb the light it catches
// (warm, as a torch's is on a face), a how much more of it shows. In the
// light's own bands; the threshold at a band's edge is the lane's, so a
// whole streak falls on one side or the other.
fn heldLightOnDrop(relative: vec3f, threshold: f32) -> vec4f {
    let band = heldLightBand(length(relative - heldLightOrigin), p.held, threshold);
    if (band <= 0.0) { return vec4f(0.0); }
    let level = heldLightLevelOfBand(band, p.held) / 15.0;
    return vec4f(vec3f(1.0, 0.93, 0.84) * level * level * 0.9, level);
}

const hidden = vec4f(0.0, 0.0, 2.0, 1.0); // beyond the far plane: clipped

@vertex
fn vs_precipitation(@builtin(vertex_index) index: u32, @builtin(instance_index) instance: u32)
    -> PrecipitationOutput {
    var out: PrecipitationOutput;
    out.position = hidden;
    out.sheet = vec2f(0.0);
    out.column = vec2f(0.0);
    out.kindAndFade = vec2f(0.0);
    out.relative = vec3f(0.0);

    let size = u32(p.corner.z);
    let column = columns[instance];
    let i = instance % size;
    let j = instance / size;
    // A block is centred on its whole number: the column's middle is there.
    let center = vec2f(p.origin.x + f32(i), p.origin.z + f32(j));
    let distance = length(center);
    let radius = f32(size) * 0.5;

    let bottom = max(column.x, p.origin.y - p.origin.w);
    let top = p.origin.y + p.origin.w;
    if (column.y < 0.5 || p.params.x <= 0.0 || bottom >= top || distance < 0.6 || distance > radius) {
        return out;
    }

    // Across the sheet: perpendicular to the line from the camera.
    let across = vec2f(-center.y, center.x) / distance;
    let side = f32(index & 1u) * 2.0 - 1.0;
    let height = select(bottom, top, (index >> 1u) == 1u);
    // Rain leans with the wind: the sheet's top moves downwind.
    let lean = select(0.0, p.params.w * (height - bottom), column.y < 1.5);
    let xz = center + across * side * 0.5 + vec2f(lean, lean * 0.3);
    out.relative = vec3f(xz.x, height - p.origin.y, xz.y);
    out.position = p.viewProjection * vec4f(out.relative, 1.0);
    out.sheet = vec2f(side * 0.5 + 0.5, height);
    out.column = vec2f(f32(p.corner.x + i32(i)), f32(p.corner.y + i32(j)));
    // Fading out toward the edge of the area, and in over the first blocks,
    // where a sheet would fill the view.
    out.kindAndFade = vec2f(column.y,
        (1.0 - smoothstep(radius * 0.55, radius, distance)) * smoothstep(0.6, 3.0, distance));
    return out;
}

fn hash(v: vec2f) -> f32 {
    return fract(sin(dot(v, vec2f(127.1, 311.7))) * 43758.5453);
}

@fragment
fn fs_precipitation(in: PrecipitationOutput) -> @location(0) vec4f {
    let intensity = p.params.x;
    let time = p.params.y;
    let light = p.params.z;
    var alpha = 0.0;
    var color = vec3f(0.0);

    if (in.kindAndFade.x < 1.5) {
        // Rain: four lanes, thin streaks about 0.7 blocks long.
        let lanes = 4.0;
        let lane = floor(in.sheet.x * lanes);
        let inLane = fract(in.sheet.x * lanes);
        let h = hash(in.column + vec2f(lane * 17.31, lane * 3.7));
        // Fewer lanes rain in a light shower.
        if (h > 0.35 + intensity * 0.65) { discard; }
        let speed = 12.0 + h * 5.0;
        let spacing = 2.5 + h * 2.5;
        let phase = fract((in.sheet.y + time * speed + h * 37.0) / spacing);
        let streak = 0.7 / spacing;
        let along = smoothstep(0.0, streak * 0.25, phase) * (1.0 - smoothstep(streak * 0.5, streak, phase));
        let offset = (h - 0.5) * 0.5;
        let width = 1.0 - smoothstep(0.015, 0.05, abs(inLane - 0.5 - offset));
        let held = heldLightOnDrop(in.relative, h);
        alpha = along * width * 0.45 * (1.0 + held.a);
        color = vec3f(0.62, 0.68, 0.76) * (light + held.rgb);
    } else {
        // Snow: three lanes of round flakes, falling slowly and swaying; five
        // in a heavy snowfall.
        let lanes = 3.0 + floor(intensity * 2.0);
        let lane = floor(in.sheet.x * lanes);
        let inLane = fract(in.sheet.x * lanes);
        let h = hash(in.column + vec2f(lane * 9.13, lane * 5.1));
        if (h > 0.2 + intensity * 0.5) { discard; }
        let speed = 1.1 + h * 0.6;
        let spacing = 2.0 + h * 1.5;
        let fall = in.sheet.y + time * speed + h * 23.0;
        let phase = fract(fall / spacing);
        let flake = floor(fall / spacing);
        let sway = sin(time * 1.3 + h * 6.28 + flake * 1.7) * 0.25;
        // In blocks: a lane is a third of a block wide.
        let dx = (inLane - 0.5 - sway) / lanes;
        let dy = (phase - 0.5) * spacing;
        // Big enough to stay whole on the PSX look's 240 lines.
        let size = 0.018 + hash(vec2f(flake, h)) * 0.02;
        let held = heldLightOnDrop(in.relative, hash(vec2f(flake, h * 7.0)));
        alpha = (1.0 - smoothstep(size, size * 1.7, length(vec2f(dx, dy)))) * 0.75 * (1.0 + held.a * 0.3);
        color = vec3f(0.95, 0.96, 1.0) * (light + held.rgb);
    }

    alpha *= intensity * in.kindAndFade.y;
    if (alpha < 0.003) { discard; }
    return vec4f(color, alpha);
}
