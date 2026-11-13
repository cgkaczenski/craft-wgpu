// Weather particles: rain and snow as a pool of real particles
// (wgpuWorld's recordPrecipitation, when Precipitation::particles is set;
// precipitation.wgsl's sheets otherwise).
//
// A fixed pool lives in a storage buffer. `simulate` moves every particle
// once a frame inside a box that follows the camera: gravity and wind pull
// its velocity toward a fall speed, snow sways, a particle that leaves the
// box sideways comes back in on the far side, and one that reaches the top
// of its column's highest blocking block (precipitation.wgsl's column map,
// so roofs, trees and cave ceilings keep it out) lands. A raindrop splashes
// for a moment, a flake rests and fades; then each is born again at the top
// of the box.
//
// `vs_weather` draws each particle as a few quads built in screen pixels: rain
// a streak along its velocity, snow a round flake facing the camera. A landed
// raindrop is a crown (a broken ring lying on the ground) and up to three
// droplets that hop away from where it hit. Each drop has its own length,
// width, brightness, crown and droplets, from the random number it was born
// with. Never thinner than a pixel, and in the PSX look the corners snap to
// its pixel grid as the world's vertices do.
//
// Positions are relative to the anchor, a whole block near the camera that
// moves in whole blocks (u.shift carries the step), so the floats stay small
// however far out the player is.

struct WeatherUniforms {
    // perspectiveRH_ZO times Camera::getViewMatrix (rotation only), as the
    // world's: camera-relative positions to clip space.
    viewProjection: mat4x4f,
    // xyz the camera from the anchor; w this frame's seconds.
    camera: vec4f,
    // xyz to add to every position (the anchor's step, whole blocks); w 1 to
    // scatter the whole pool afresh.
    shift: vec4f,
    // x the box's half width, y how far above and below the camera it
    // reaches, z how many particles are drawn, w the pool's size.
    box: vec4f,
    // xy column (0, 0)'s block x and z from the anchor; z columns per side;
    // w a frame counter (for the random numbers).
    corner: vec4i,
    // x the intensity (0..1), y seconds, z the brightness of the light the
    // drops catch, w the anchor's world y.
    params: vec4f,
    // x the rain's lean (blocks sideways per block down); yzw unused.
    wind: vec4f,
    // The light of what the player holds, as world.wgsl's u.heldLight.
    held: vec4f,
    // x 1 in the PSX look; y the quads drawn per particle; zw unused.
    psx: vec4f,
    // xy the pixels across and down (the PSX grid in the PSX look, the
    // surface otherwise); zw the projection's x and y scale.
    pixels: vec4f,
};

// position: xyz from the anchor; w < 0 waiting to be born (no rain or snow
// where it was to start), 0..landed seconds since it was born, falling;
// landed and more, landed (w - landed seconds ago).
// velocity: xyz blocks a second; w the kind (1 rain, 2 snow) plus a random
// number 0..1 in the fraction, its own for this life.
struct Particle {
    position: vec4f,
    velocity: vec4f,
};

@group(0) @binding(0) var<uniform> u: WeatherUniforms;
// Per column: x the world y the rain stops at, y 0 none, 1 rain, 2 snow.
@group(0) @binding(1) var<storage, read> columns: array<vec2f>;
@group(0) @binding(2) var<storage, read_write> particles: array<Particle>;
// The same pool, read by the draw.
@group(0) @binding(3) var<storage, read> drawn: array<Particle>;

#include "heldLight.wgsl"

// How long a landed raindrop stays landed: its crown and droplets are over
// by then (crownSeconds and dropletFlight's longest).
const rainLandedSeconds = 0.26;
const landed = 1000.0;
const fadeInSeconds = 0.4;
const snowRestSeconds = 1.4;

// ---- Random numbers -------------------------------------------------------

fn pcg(v: u32) -> u32 {
    let state = v * 747796405u + 2891336453u;
    let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Four numbers 0..1 for particle `i` this frame.
fn random4(i: u32) -> vec4f {
    let a = pcg(i ^ pcg(u32(u.corner.w)));
    let b = pcg(a);
    let c = pcg(b);
    let d = pcg(c);
    return vec4f(f32(a), f32(b), f32(c), f32(d)) / 4294967295.0;
}

// Four numbers 0..1 for a particle's life, from its seed: the same every
// frame, different for each `salt`.
fn randomOf(seed: f32, salt: u32) -> vec4f {
    let a = pcg(u32(seed * 16777215.0) + salt * 2654435769u);
    let b = pcg(a);
    let c = pcg(b);
    let d = pcg(c);
    return vec4f(f32(a), f32(b), f32(c), f32(d)) / 4294967295.0;
}

// ---- The simulation -------------------------------------------------------

// The column under a point (from the anchor): x the height the rain stops
// at, from the anchor; y the kind, 0 outside the map. A block is centred on
// its whole number, so block x's column runs from x - 0.5 to x + 0.5.
fn columnAt(xz: vec2f) -> vec2f {
    let size = u.corner.z;
    let i = i32(floor(xz.x + 0.5)) - u.corner.x;
    let j = i32(floor(xz.y + 0.5)) - u.corner.y;
    if (i < 0 || j < 0 || i >= size || j >= size) { return vec2f(0.0); }
    let column = columns[u32(i + j * size)];
    return vec2f(column.x - u.params.w, column.y);
}

// How fast a particle of this kind wants to go, at this time.
fn fallVelocity(kind: f32, seed: f32, time: f32) -> vec3f {
    let lean = u.wind.x;
    if (kind < 1.5) {
        let speed = 11.0 + seed * 5.0;
        return vec3f(lean * speed, -speed, lean * 0.3 * speed);
    }
    // Snow: a slow fall, a drift downwind and two swirls of its own.
    let phase = seed * 61.7;
    let drift = 0.25 + lean * 3.0;
    let sway = vec2f(
        sin(time * 0.9 + phase) * 0.45 + sin(time * 2.3 + phase * 1.7) * 0.15,
        cos(time * 0.7 + phase * 1.3) * 0.45 + cos(time * 1.9 + phase * 0.6) * 0.15);
    return vec3f(drift + sway.x, -(0.9 + seed * 0.6), drift * 0.3 + sway.y);
}

// A particle born at random in the box: anywhere between its column's floor
// and the top when the pool is scattered (or it was waiting: it fades in),
// just above the top otherwise.
fn born(i: u32, scatter: bool) -> Particle {
    var p: Particle;
    let r = random4(i);
    let half = u.box.x;
    let top = u.camera.y + u.box.y;
    let bottom = u.camera.y - u.box.y;
    let xz = u.camera.xz + (r.xy * 2.0 - 1.0) * half;
    let column = columnAt(xz);
    p.position = vec4f(xz.x, top, xz.y, -1.0);
    p.velocity = vec4f(0.0);
    let ground = max(column.x, bottom);
    if (column.y < 0.5 || ground >= top) { return p; } // nothing falls here: wait
    let y = select(top + r.z * 2.0, mix(ground, top, r.z), scatter);
    let kind = column.y;
    p.position = vec4f(xz.x, y, xz.y, 0.0);
    p.velocity = vec4f(fallVelocity(kind, r.w, u.params.y), kind + r.w * 0.999);
    return p;
}

@compute @workgroup_size(64)
fn simulate(@builtin(global_invocation_id) id: vec3u) {
    let i = id.x;
    if (i >= u32(u.box.w)) { return; }
    if (u.shift.w > 0.5) { particles[i] = born(i, true); return; }

    var p = particles[i];
    p.position = vec4f(p.position.xyz + u.shift.xyz, p.position.w);
    let dt = u.camera.w;

    // Waiting to be born: try again, somewhere else, at any height (so a
    // pool that waited for the chunks to load fills the whole box).
    if (p.position.w < 0.0) { particles[i] = born(i, true); return; }

    let kind = floor(p.velocity.w);
    let seed = fract(p.velocity.w);

    // Landed: splash or rest, then start over.
    if (p.position.w >= landed) {
        p.position.w += dt;
        let life = select(snowRestSeconds, rainLandedSeconds, kind < 1.5);
        if (p.position.w - landed >= life) { p = born(i, false); }
        particles[i] = p;
        return;
    }

    // Falling: toward the fall speed (rain quickly, snow gently), then a
    // step.
    let goal = fallVelocity(kind, seed, u.params.y);
    let rate = select(1.5, 6.0, kind < 1.5);
    var velocity = mix(p.velocity.xyz, goal, 1.0 - exp(-rate * dt));
    var position = p.position.xyz + velocity * dt;

    // Out of the box sideways: in again on the far side.
    let half = u.box.x;
    let fromCamera = position.xz - u.camera.xz;
    let wrapped = fromCamera - 2.0 * half * floor((fromCamera + half) / (2.0 * half));
    let didWrap = any(abs(wrapped - fromCamera) > vec2f(0.001));
    position = vec3f(u.camera.x + wrapped.x, position.y, u.camera.z + wrapped.y);

    let column = columnAt(position.xz);
    let bottom = u.camera.y - u.box.y;
    let top = u.camera.y + u.box.y;
    if (column.y < 0.5 || position.y < bottom || position.y > top + 3.0
        || (position.y <= column.x && (didWrap || p.position.y < column.x))) {
        // Into a column with nothing falling, out of the box, wrapped under
        // a roof, or blown into the side of a taller column (it was already
        // under that column's top): start over.
        p = born(i, false);
    } else if (position.y <= column.x) {
        // Reached the column's top: land on it.
        p.position = vec4f(position.x, column.x, position.z, landed);
        p.velocity = vec4f(velocity, p.velocity.w);
    } else {
        // Snow over a rain column (or rain over snow) turns into it.
        let newKind = column.y;
        p.position = vec4f(position, min(p.position.w + dt, landed - 1.0));
        p.velocity = vec4f(velocity, newKind + seed);
    }
    particles[i] = p;
}

// ---- The draw -------------------------------------------------------------

struct WeatherOutput {
    @builtin(position) position: vec4f,
    @location(0) local: vec2f,                         // across and along the quad, -1..1
    // x what it is (1 a streak, 2 a flake, 3 a crown, 4 a far splash: a dot),
    // y alpha, z seed, w how far through its life a crown is (0..1).
    @location(1) @interpolate(flat) look: vec4f,
    @location(2) relative: vec3f,                      // from the camera, for the held light
};

const hidden = vec4f(0.0, 0.0, 2.0, 1.0); // beyond the far plane: clipped
const nearest = 0.05; // nearer than this, a particle is not drawn
const landedDepthBias = 0.2; // blocks toward the camera, for a landed particle's depth

// A clip-space point to pixels (from the screen's centre), and back.
fn toPixels(clip: vec4f) -> vec2f {
    return clip.xy / clip.w * u.pixels.xy * 0.5;
}
fn fromPixels(pixels: vec2f, clip: vec4f) -> vec4f {
    // The PSX look: on its grid's pixel corners, as world.wgsl's psxSnap.
    let snapped = select(pixels, round(pixels), u.psx.x > 0.5);
    return vec4f(snapped / (u.pixels.xy * 0.5) * clip.w, clip.zw);
}

// The quad's corner for `index` (0..3, a strip): x -1 or 1, y -1 or 1.
fn corner(index: u32) -> vec2f {
    return vec2f(f32(index & 1u) * 2.0 - 1.0, f32(index >> 1u) * 2.0 - 1.0);
}

// How big `blocks` is at `w`, in pixels.
fn blocksToPixels(blocks: f32, w: f32) -> f32 {
    return blocks * u.pixels.w * u.pixels.y * 0.5 / w;
}

// A streak: a quad from `head` (from the camera) back along `velocity` for
// `seconds` of it, `width` blocks wide; at least as long as it is wide, and
// never thinner than `minimum` pixels. Hidden when too near the camera.
fn streak(head3: vec3f, velocity: vec3f, seconds: f32, width: f32, minimum: f32, c: vec2f) -> vec4f {
    let head = u.viewProjection * vec4f(head3, 1.0);
    let tail = u.viewProjection * vec4f(head3 - velocity * seconds, 1.0);
    if (head.w < nearest || tail.w < nearest) { return hidden; }
    let headPixels = toPixels(head);
    var tailPixels = toPixels(tail);
    let wide = max(blocksToPixels(width, head.w), minimum);
    let along = headPixels - tailPixels;
    let span = length(along);
    let direction = select(vec2f(0.0, 1.0), along / max(span, 0.0001), span > 0.0001);
    if (span < wide) { tailPixels = headPixels - direction * wide; }
    let across = vec2f(-direction.y, direction.x);
    let end = select(tail, head, c.y > 0.0);
    let endPixels = select(tailPixels, headPixels, c.y > 0.0);
    return fromPixels(endPixels + across * c.x * wide * 0.5, end);
}

// A landed raindrop's crown lasts this long, by its random number (0..1).
fn crownSeconds(r: f32) -> f32 { return 0.09 + 0.13 * r; }

// The droplets a landed raindrop throws: how hard they fall back, and how
// fast one leaves the ground, by its random number. The longest flight
// (2 * speed / gravity) is 0.25 s, inside rainLandedSeconds.
const dropletGravity = 24.0;
fn dropletSpeedUp(r: f32) -> f32 { return 1.0 + 2.0 * r; }

@vertex
fn vs_weather(@builtin(vertex_index) index: u32, @builtin(instance_index) instance: u32) -> WeatherOutput {
    var out: WeatherOutput;
    out.position = hidden;
    out.local = vec2f(0.0);
    out.look = vec4f(0.0);
    out.relative = vec3f(0.0);

    // Each particle has a few quads: the first is the drop, the flake or the
    // crown; the others are a landed raindrop's droplets.
    let quads = max(u32(u.psx.y), 1u);
    let particle = instance / quads;
    let part = instance % quads;
    let p = drawn[particle];
    if (f32(particle) >= u.box.z || p.position.w < 0.0) { return out; }

    let kind = floor(p.velocity.w);
    let seed = fract(p.velocity.w);
    let relative = p.position.xyz - u.camera.xyz;
    out.relative = relative;
    let c = corner(index);
    out.local = c;

    // Fading out toward the box's sides, and right at the camera.
    let away = length(relative.xz);
    let isLanded = p.position.w >= landed;
    let isRain = kind < 1.5;
    let age = select(p.position.w, fadeInSeconds, isLanded);
    let fade = (1.0 - smoothstep(u.box.x * 0.6, u.box.x, away)) * smoothstep(0.25, 0.8, length(relative))
        * smoothstep(0.0, fadeInSeconds, age);
    let minimum = select(1.0, 1.2, u.psx.x > 0.5); // pixels: never thinner than one
    // This drop's own numbers, the same for its whole life.
    let own = randomOf(seed, 0u);

    if (part > 0u) {
        // A droplet: thrown from where the drop hit, up and outward on an
        // arc of its own, and gone when it comes down again. Not simulated:
        // where it is follows from how long ago the drop landed. A heavy
        // drop (own.y) throws more of them.
        if (!isRain || !isLanded) { return out; }
        let r = randomOf(seed, part);
        if (r.x > 0.3 + 0.6 * own.y) { return out; }
        let since = p.position.w - landed;
        let up = dropletSpeedUp(r.y);
        let flight = 2.0 * up / dropletGravity;
        if (since >= flight) { return out; }
        let angle = r.w * 6.2831853;
        // Outward, and a little of the way the drop was going (the wind's).
        let outward = vec2f(cos(angle), sin(angle)) * (0.3 + 1.3 * r.z) + p.velocity.xz * 0.08;
        let velocity = vec3f(outward.x, up - dropletGravity * since, outward.y);
        let at = relative + vec3f(outward.x * since,
            0.01 + up * since - 0.5 * dropletGravity * since * since, outward.y * since);
        out.relative = at;
        out.position = streak(at, velocity, 0.025, 0.012, minimum, c);
        out.look = vec4f(1.0, (0.35 + 0.3 * r.x) * (1.0 - smoothstep(0.6, 1.0, since / flight)) * fade, seed, 0.0);
        return out;
    }

    if (isRain && !isLanded) {
        // Rain: a streak from the drop back along its velocity. Each drop
        // has its own length (0.03 to 0.07 s of its fall: 0.3 to 1.1
        // blocks), width (most thin, a few heavy) and brightness.
        out.position = streak(relative, p.velocity.xyz, 0.03 + 0.04 * own.x,
            0.009 + 0.014 * own.y * own.y, minimum, c);
        out.look = vec4f(1.0, (0.3 + 0.35 * own.z) * fade, seed, 0.0);
        return out;
    }

    var center = u.viewProjection * vec4f(relative, 1.0);
    if (center.w < nearest) { return out; }
    let centerPixels = toPixels(center);
    if (isLanded) {
        // On the ground: the quad faces the camera, so its lower half would
        // be behind the face it landed on. Its depth is that of a point a
        // little nearer the camera, so the whole of it shows.
        let reach = length(relative);
        let nearer = u.viewProjection * vec4f(relative * (1.0 - min(landedDepthBias / reach, 0.5)), 1.0);
        center.z = nearer.z / nearer.w * center.w;
    }

    if (isRain) {
        // A crown: a ring lying flat on the ground (so as thin as the
        // ground is seen edge on) that bursts outward, quickly at first,
        // and fades. Each drop's has its own size (most small, a heavy
        // drop's large), life and brightness. Too far to be a ring, a dot.
        let t = (p.position.w - landed) / crownSeconds(own.x);
        if (t >= 1.0) { return out; }
        let widest = 0.03 + 0.09 * own.y * own.y;
        let blocks = widest * (0.25 + 0.75 * sqrt(t));
        let radius = max(blocksToPixels(blocks, center.w), minimum * 0.75);
        let tilt = abs(relative.y) / max(length(relative), 0.0001);
        let size = vec2f(radius, max(radius * tilt, minimum * 0.6));
        out.position = fromPixels(centerPixels + c * size, center);
        out.look = vec4f(select(3.0, 4.0, radius < 2.5), (0.3 + 0.3 * own.z) * (1.0 - t) * fade, seed, t);
        return out;
    }

    // Snow: a round flake facing the camera; resting, it fades away.
    let rest = select(1.0, 1.0 - (p.position.w - landed) / snowRestSeconds, isLanded);
    let radius = max(blocksToPixels(0.03 + seed * 0.03, center.w), minimum * 0.75);
    out.position = fromPixels(centerPixels + c * radius, center);
    out.look = vec4f(2.0, 0.85 * rest * fade, seed, 0.0);
    return out;
}

// What the held light adds to a drop or a flake, as precipitation.wgsl's.
fn heldLightOnDrop(relative: vec3f, threshold: f32) -> vec4f {
    let band = heldLightBand(length(relative - heldLightOrigin), u.held, threshold);
    if (band <= 0.0) { return vec4f(0.0); }
    let level = heldLightLevelOfBand(band, u.held) / 15.0;
    return vec4f(vec3f(1.0, 0.93, 0.84) * level * level * 0.9, level);
}

@fragment
fn fs_weather(in: WeatherOutput) -> @location(0) vec4f {
    let light = u.params.z;
    let held = heldLightOnDrop(in.relative, in.look.z);
    var alpha = in.look.y;
    var color: vec3f;

    if (in.look.x < 1.5) {
        // Rain (and a splash's droplets): fainter toward the tail.
        alpha *= mix(0.3, 1.0, in.local.y * 0.5 + 0.5) * (1.0 + held.a);
        color = vec3f(0.62, 0.68, 0.76) * (light + held.rgb);
    } else if (in.look.x < 2.5) {
        // Snow: round, soft at the rim.
        alpha *= (1.0 - smoothstep(0.55, 1.0, length(in.local))) * (1.0 + held.a * 0.3);
        color = vec3f(0.95, 0.96, 1.0) * (light + held.rgb);
    } else if (in.look.x < 3.5) {
        // A crown: a rim that thins as it spreads, broken into a few arcs
        // (how many and where is the drop's own), not a clean circle.
        let r = length(in.local);
        let t = in.look.w;
        let seed = in.look.z;
        let inner = mix(0.2, 0.72, t);
        let rim = smoothstep(inner, inner + 0.18, r) * (1.0 - smoothstep(0.82, 1.0, r));
        let angle = atan2(in.local.y, in.local.x);
        let arcs = 0.5 + 0.5 * sin(angle * (3.0 + floor(seed * 4.0)) + seed * 97.0);
        alpha *= rim * mix(1.0, smoothstep(0.15, 0.6, arcs), 0.75) * (1.0 + held.a * 0.5);
        color = vec3f(0.7, 0.75, 0.82) * (light + held.rgb);
    } else {
        // A far splash: a dot.
        alpha *= (1.0 - smoothstep(0.6, 1.0, length(in.local))) * (1.0 + held.a * 0.5);
        color = vec3f(0.7, 0.75, 0.82) * (light + held.rgb);
    }

    if (alpha < 0.003) { discard; }
    return vec4f(color, alpha);
}
