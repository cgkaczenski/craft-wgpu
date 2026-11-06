// Milestone 14: the sky, the sun and the moon (skyBoxRenderer.cpp's
// SkyBoxLoaderAndDrawer and SunRenderer; skybox/atmosphericScattering.frag,
// skyBox.frag, sun.vert, sun.frag). Three uses:
//
//   fs_bake   once, and again when the sky settings change: GL's three
//             skies (day, twilight, night), each into the six faces of a
//             128x128 cubemap. One draw per face, its instance naming
//             the sky and the face (sky * 6 + face).
//   fs_sky    every frame, before the world: the time of day's two skies
//             blended, into the HDR color and into a screen-sized copy that
//             the fog of the lit shaders reads (GL's fboSkyBox).
//   vs_sun    every frame, after the entities: the sun and the moon,
//   fs_sun    textured quads at the far plane, added onto what is left of
//             the sky. Instance 0 is the sun, 1 the moon.
//   fs_sunRays  milestone 18: the two again (instances 2 and 3, with GL's
//             god-ray settings) at half size, for the god rays; the sun
//             also counts its pixels in front of the sky, for the flare.

// ---- The bake (atmosphericScattering.frag) --------------------------------

// One sky's settings, as SkyBoxLoaderAndDrawer::createSkyTextures passes
// them to atmosphericScattering.
struct BakeParams {
    lightPos: vec4f,    // xyz u_lightPos
    color1: vec4f,      // rgb u_color1, the sky
    color2: vec4f,      // rgb u_color2, the light near the horizon
    groundColor: vec4f, // rgb u_groundColor; w u_g
    flags: vec4u,       // x u_useGround
};
@group(0) @binding(0) var<storage, read> bakes: array<BakeParams>;

const bakeSize = 128.0;

struct FullscreenOutput {
    @builtin(position) position: vec4f,
    @location(0) ndc: vec2f,
    @location(1) @interpolate(flat) instance: u32,
};

// One triangle covering the target, as gbufferView.wgsl's.
@vertex
fn vs_fullscreen(@builtin(vertex_index) index: u32, @builtin(instance_index) instance: u32) -> FullscreenOutput {
    let x = f32((index << 1u) & 2u) * 2.0 - 1.0;
    let y = f32(index & 2u) * 2.0 - 1.0;
    var out: FullscreenOutput;
    out.position = vec4f(x, y, 0.0, 1.0);
    out.ndc = vec2f(x, y);
    out.instance = instance;
    return out;
}

// The direction a texel of a cube face stands for. Sampling picks the face
// by the direction's largest axis and then (sc, tc) by this table (the
// cube map rules OpenGL, Vulkan, Metal and WebGPU share); here it is run
// backward. s runs right and t down the face, both 0..1, row 0 at t = 0.
// GL renders its faces through lookAt matrices instead, whose odd up
// vectors make up for OpenGL's bottom-up framebuffers; WebGPU's run
// top-down, so the table is used rather than those matrices.
fn cubeDirection(face: u32, s: f32, t: f32) -> vec3f {
    let sc = 2.0 * s - 1.0;
    let tc = 2.0 * t - 1.0;
    switch face {
        case 0u: { return vec3f(1.0, -tc, -sc); }  // +X
        case 1u: { return vec3f(-1.0, -tc, sc); }  // -X
        case 2u: { return vec3f(sc, 1.0, tc); }    // +Y
        case 3u: { return vec3f(sc, -1.0, -tc); }  // -Y
        case 4u: { return vec3f(sc, -tc, 1.0); }   // +Z
        default: { return vec3f(-sc, -tc, -1.0); } // -Z
    }
}

// GL's modified O'Neil scattering. GL sets fCosSun to 0 whatever u_noSun
// says, so the Mie term is a constant and the sky depends only on how high
// the direction points: the sky color, plus the horizon light rising
// toward the horizon (the more so the lower the light is).
fn scatter(direction: vec3f, p: BakeParams) -> vec3f {
    let g = p.groundColor.w;
    let localPos = normalize(direction);
    let lightPos = normalize(p.lightPos.xyz);
    let up = vec3f(0.0, 1.0, 0.0);
    let cosEarth = max(dot(localPos, up), 0.0);
    let oneMinusCosEarth = 1.0 - cosEarth;
    let cosSunEarth = 1.0 - abs(dot(lightPos, up));

    let g2 = g * g;
    let cosSun = 0.0;
    let miePhase = 1.5 * ((1.0 - g2) / (2.0 + g2)) * (1.0 + cosSun * cosSun) / pow(1.0 + g2 - 2.0 * g * cosSun, 1.5);

    let horizon = pow(oneMinusCosEarth, 16.0);
    let sky = p.color1.rgb + miePhase * p.color2.rgb + cosSunEarth * p.color2.rgb * horizon + horizon * p.color2.rgb;

    if (cosEarth < 0.02 && p.flags.x != 0u) {
        var a = min(max(cosEarth / 0.02, 0.0), 1.0);
        a = a * a * a;
        return mix(p.groundColor.rgb, sky, vec3f(a));
    }
    return sky;
}

@fragment
fn fs_bake(in: FullscreenOutput) -> @location(0) vec4f {
    let direction = cubeDirection(in.instance % 6u, in.position.x / bakeSize, in.position.y / bakeSize);
    return vec4f(scatter(direction, bakes[in.instance / 6u]), 1.0);
}

// ---- Every frame -----------------------------------------------------------

struct SkyUniforms {
    // The camera's rotation-only view times its 0..1 projection, inverted:
    // a pixel back to its direction.
    inverseViewProjection: mat4x4f,
    // How much of each sky (day, twilight, night) this time of day shows:
    // drawBefore's two textures and mix, as weights. w unused.
    weights: vec4f,
    // The sun's and the moon's SunRenderer matrices (GL clip space; only
    // x, y and w are used, z is put at the far plane).
    sunMatrix: mat4x4f,
    moonMatrix: mat4x4f,
    // x u_sunset, y u_colorMultiplier, for the sun then the moon.
    sunParams: vec4f,
    moonParams: vec4f,
    // The same for the god rays' sun and moon (milestone 18): GL draws
    // them again with the twilight at least 0.5 and the moon at 0.35.
    raySunParams: vec4f,
    rayMoonParams: vec4f,
    // Milestone G1, the weather: x how far the sky goes to grey (overcast,
    // 0..1), y how much darker that grey is (0..1).
    weather: vec4f,
};
@group(0) @binding(0) var<uniform> sky: SkyUniforms;
@group(0) @binding(1) var daySky: texture_cube<f32>;
@group(0) @binding(2) var twilightSky: texture_cube<f32>;
@group(0) @binding(3) var nightSky: texture_cube<f32>;
@group(0) @binding(4) var skySampler: sampler; // linear, clamped
@group(0) @binding(5) var sunTexture: texture_2d<f32>;
@group(0) @binding(6) var moonTexture: texture_2d<f32>;

// Two targets, the same color: the sky copy, then the HDR color.
struct SkyTargets {
    @location(0) sky: vec4f,
    @location(1) color: vec4f,
};

// skyBox.frag. GL also turns each sky about the vertical by clock(); the
// skies change only with height, so that turn shows nothing and is left out.
@fragment
fn fs_sky(in: FullscreenOutput) -> SkyTargets {
    let far = sky.inverseViewProjection * vec4f(in.ndc, 1.0, 1.0);
    let direction = normalize(far.xyz / far.w);
    var color = (textureSample(daySky, skySampler, direction).rgb * sky.weights.x
        + textureSample(twilightSky, skySampler, direction).rgb * sky.weights.y
        + textureSample(nightSky, skySampler, direction).rgb * sky.weights.z) * 1.2;
    color = overcastSky(color);
    var out: SkyTargets;
    out.sky = vec4f(color, 1.0);
    out.color = vec4f(color, 1.0);
    return out;
}

struct SunOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) @interpolate(flat) instance: u32,
};

// SunRenderer's quad: its corners at z = -4 in the frame createTBNMatrix
// turns toward the sun, so 4 units out along the sun's direction, as a
// strip. The depth goes to the far plane (gl_Position.z = w), so anything
// drawn hides it.
// Milestone G1: clouds. The sky toward a grey as bright as it was on average
// (so the time of day still shows: a grey day, a darker grey dusk, a dark
// night), a little blue, darker by the weather's skyDarken. The fog of the lit
// shaders reads this sky, so it greys with it.
fn overcastSky(color: vec3f) -> vec3f {
    let overcast = sky.weather.x;
    if (overcast <= 0.0) { return color; }
    let brightness = dot(color, vec3f(1.0 / 3.0));
    let grey = vec3f(brightness) * vec3f(0.94, 0.97, 1.0) * (1.0 - sky.weather.y);
    return mix(color, grey, overcast);
}

@vertex
fn vs_sun(@builtin(vertex_index) index: u32, @builtin(instance_index) instance: u32) -> SunOutput {
    let corner = vec2f(f32(index & 1u) * 2.0 - 1.0, f32(index >> 1u) * 2.0 - 1.0);
    var matrix = sky.sunMatrix;
    if ((instance & 1u) == 1u) { matrix = sky.moonMatrix; }
    var out: SunOutput;
    out.position = matrix * vec4f(corner, -4.0, 1.0);
    out.position.z = out.position.w;
    // GL's uv runs up with y; wgpu2d's textures are stored top row first.
    out.uv = vec2f(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    out.instance = instance;
    return out;
}

fn rgbToHsv(c: vec3f) -> vec3f {
    let K = vec4f(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    let p = mix(vec4f(c.bg, K.wz), vec4f(c.gb, K.xy), step(c.b, c.g));
    let q = mix(vec4f(p.xyw, c.r), vec4f(c.r, p.yzx), step(p.x, c.r));
    let d = q.x - min(q.w, q.y);
    let e = 1.0e-10;
    return vec3f(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

fn hsvToRgb(c: vec3f) -> vec3f {
    let K = vec4f(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    let p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, vec3f(0.0), vec3f(1.0)), c.y);
}

// The G-buffer's four targets: the color added (SrcAlpha, One), positions
// and normals left alone (masked), the bloom written over.
struct SunTargets {
    @location(0) color: vec4f,
    @location(1) position: vec4f,
    @location(2) normal: vec4u,
    @location(3) bloom: vec4f,
};

// sun.frag: the texture's hue turned toward red at twilight, brightened.
// Instances 0 and 1 are the sun and moon, 2 and 3 their god-ray copies.
fn sunColor(in: SunOutput) -> vec4f {
    // Both read, one kept: textureSample must stay in uniform control flow.
    let sunTexel = textureSample(sunTexture, skySampler, in.uv);
    let moonTexel = textureSample(moonTexture, skySampler, in.uv);
    var c = sunTexel;
    var params = sky.sunParams;
    if (in.instance == 1u) { c = moonTexel; params = sky.moonParams; }
    if (in.instance == 2u) { params = sky.raySunParams; }
    if (in.instance == 3u) { c = moonTexel; params = sky.rayMoonParams; }

    var hsv = rgbToHsv(c.rgb);
    hsv.x -= params.x * 30.0 / 180.0;
    if (hsv.x < 0.0) { hsv.x += 1.0; }
    hsv.y += params.x * 2.0 / 180.0;
    if (hsv.y > 1.0) { hsv.x = 1.0; } // as GL (it sets the hue, not the saturation)
    var color = vec4f(hsvToRgb(hsv), c.a);
    return vec4f(color.rgb * 1.5 * params.y, color.a);
}

@fragment
fn fs_sun(in: SunOutput) -> SunTargets {
    let color = sunColor(in);
    var out: SunTargets;
    out.color = color;
    out.position = vec4f(0.0);
    out.normal = vec4u(0u);
    out.bloom = vec4f(color.rgb * 0.014, 0.0);
    return out;
}

// ---- Milestone 18: the god rays' sun and moon, and the flare's count -----

// The scene's depth (opaque and entities: GL draws this before the
// transparent faces) and the count of the sun's visible pixels.
@group(1) @binding(0) var sceneDepth: texture_depth_2d;
@group(1) @binding(1) var<storage, read_write> sunSamples: atomic<u32>;

// GL draws these with blending off and no depth test, into a half-size
// target cleared to black (fboSunForGodRays), so the color is written as is.
// The sun's quad also stands in for GL's occlusion query (GL_SAMPLES_PASSED
// around the full-size sun, depth LessEqual at the far plane): WebGPU's
// occlusion queries only promise zero or not, so each half-size pixel
// counts the full-size pixels under it where the depth is the far plane's,
// the depth test done by hand (a shader that writes memory cannot rely on
// the test having run first).
@fragment
fn fs_sunRays(in: SunOutput) -> @location(0) vec4f {
    let color = sunColor(in);
    if (in.instance == 2u) {
        let size = vec2i(textureDimensions(sceneDepth));
        let base = vec2i(in.position.xy) * 2;
        var visible = 0u;
        for (var y = 0; y < 2; y++) {
            for (var x = 0; x < 2; x++) {
                let texel = base + vec2i(x, y);
                if (all(texel < size) && textureLoad(sceneDepth, texel, 0) >= 1.0) { visible += 1u; }
            }
        }
        if (visible > 0u) { atomicAdd(&sunSamples, visible); }
    }
    return vec4f(color.rgb, 1.0);
}
