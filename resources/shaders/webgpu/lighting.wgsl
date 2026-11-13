// Milestone 12: defaultShader.frag's lighting, translated for the chunk faces
// (chunkFaces.wgsl includes it after world.wgsl). GL's lit path, the one its
// default settings take (`shaders` on):
//
//   ambient   the face's baked light levels (sky light scaled by the time
//             of day, block light), lifted and put through the sRGB curve,
//             tinted, darkened by the corner AO texture: computeAmbient
//   sun       physically based: GGX distribution, Smith geometry, Schlick
//             Fresnel, Oren-Nayar diffuse, with the block's normal and
//             material maps: computePointLightSource
//   lights    the same model for each nearby light block, gated by GL's
//             distance and light-level tests: pointLights
//   paint     painted blocks recolored through HSV: paintAlbedo
//   shadow    the sun's cascaded shadow maps (milestone 13), on the sun term
//             and on the sky part of the ambient: sunShadow
//
// The fog toward the sky (milestone 14), the reflections (milestone 17) and
// the underwater darkening (milestone 18) are chunkFaces.wgsl's, as are the
// water's moving normal and the caustics (18-parity), which reach the lights
// here as their `caustics` tint. GL's BRDF LUT only feeds its physically
// accurate ambient, which a constant switches off
// (physicallyAccurateReflections), so its output never reads the LUT and
// neither does this.

const PI = 3.14159265359;

// The point lights: each chunk's light blocks (Chunk's `lights`), the
// nearest chunks' gathered by wgpuWorld up to the renderer's maxLights.
// xyz a light's block position; w unused. u.lightParams.y says how many.
@group(0) @binding(6) var<storage, read> lights: array<vec4i>;

// GL's u_ao: the corner darkening texture (ao.png), mirrored in u.
@group(0) @binding(7) var aoTexture: texture_2d<f32>;
@group(0) @binding(8) var aoSampler: sampler;

// ---- Sun shadows (milestone 13; defaultShader.frag's shadowCalc2) --------

// The cascades (GL's u_cascadedShadowsMaps): depth seen from the sun,
// nearest cascade first, 2048, 2048 and 1024 texels square. While shadows
// are off, and in the shadow passes themselves, a 1x1 stand-in is bound.
@group(0) @binding(9) var shadowMap0: texture_depth_2d;
@group(0) @binding(10) var shadowMap1: texture_depth_2d;
@group(0) @binding(11) var shadowMap2: texture_depth_2d;
// Nearest, LessEqual: a tap is 1 where the reference is no deeper than the
// depth stored there, 0 where something nearer the sun covers it.
@group(0) @binding(12) var shadowSampler: sampler_comparison;

// The bias is in the map's depth, which spans the cascade's box and 500
// blocks either side of it (Renderer's sunCascadeMatrix): about 1050, 1120
// and 2000 blocks. So the far cascade's 0.005 is 10 blocks: nothing within
// 10 blocks of a face shades it there, and a house has no shadows of its
// own. That is on purpose: its map is about a block to the texel, and with
// a bias small enough for such shadows a roof's shadow on a wall is a row
// of block-sized teeth. sunShadow fades the nearer cascade's shadows in
// instead.
fn shadowBias(dotLightNormal: f32, cascade: u32) -> f32 {
    if (cascade == 0u) { return max(0.0002, 0.0007 * (1.0 - dotLightNormal)); }
    if (cascade == 1u) { return max(0.0003, 0.0010 * (1.0 - dotLightNormal)); }
    return max(0.005, 0.0016 * (1.0 - dotLightNormal));
}

// One tap of GL's test, (depth + bias) < z ? 0 : 1, as a comparison against
// z - bias. Off the map is lit: GL clamps to a border of depth 1, and
// WebGPU has no border color. The Level form takes no derivatives, so it is
// allowed in branches and loops (textureSampleCompare is not).
fn shadowTap(cascade: u32, uv: vec2f, reference: f32) -> f32 {
    if (any(uv < vec2f(0.0)) || any(uv > vec2f(1.0))) { return 1.0; }
    switch cascade {
        case 0u: { return textureSampleCompareLevel(shadowMap0, shadowSampler, uv, reference); }
        case 1u: { return textureSampleCompareLevel(shadowMap1, shadowSampler, uv, reference); }
        default: { return textureSampleCompareLevel(shadowMap2, shadowSampler, uv, reference); }
    }
}

fn shadowMapSize(cascade: u32) -> vec2f {
    switch cascade {
        case 0u: { return vec2f(textureDimensions(shadowMap0)); }
        case 1u: { return vec2f(textureDimensions(shadowMap1)); }
        default: { return vec2f(textureDimensions(shadowMap2)); }
    }
}

// GL's InterleavedGradientNoise and vogelDiskSample (from the gamedev.net
// article on contact-hardening shadows that GL cites): a per-pixel angle,
// and points spread evenly over a disk turned by it.
// (GL keeps the three numbers in one vec3 and swizzles it; naga's override
// pass trips over that, "ExpressionAlreadyInScope", so they are literals.)
fn interleavedGradientNoise(p: vec2f) -> f32 {
    return fract(52.9829189 * fract(dot(p, vec2f(0.06711056, 0.00583715))));
}

fn vogelDiskSample(index: i32, count: i32, phi: f32) -> vec2f {
    let goldenAngle = 2.4;
    let r = sqrt(f32(index) + 0.5) / sqrt(f32(count));
    let theta = f32(index) * goldenAngle + phi;
    return vec2f(r * cos(theta), r * sin(theta));
}

// How much of the sun reaches a point by one cascade's map, 0-1: GL's
// shadowCalc2. `relative` is the camera-relative position and `fragCoord`
// the pixel, which turns the soft disk.
fn cascadeShadow(cascade: u32, relative: vec3f, dotLightNormal: f32, fragCoord: vec2f, mode: i32) -> f32 {
    // To the map's texels: x right, y down (render targets are top-first).
    // The depth is already 0..1, the same number as GL's z * 0.5 + 0.5, so
    // GL's biases carry over.
    let clip = u.shadowMatrices[cascade] * vec4f(relative, 1.0);
    let uv = vec2f(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    let z = min(clip.z, 1.0);
    let reference = z - shadowBias(dotLightNormal, cascade);

    // Hard: one tap (GL's shadowCalc).
    if (mode == 1) { return shadowTap(cascade, uv, reference); }

    // Soft: nine taps on a turned disk 1.5 texels wide; the outer five
    // first, and when they agree the point is fully in or out of shadow and
    // the rest are skipped. The power darkens the penumbra.
    if (z > 0.99995) { return 1.0; }
    let texel = 1.0 / shadowMapSize(cascade);
    let sampleCount = 9;
    let checkCount = 5;
    let spread = 1.5;
    let phi = interleavedGradientNoise(fragCoord) * 2.0 * PI;
    var shadow = 0.0;
    for (var i = sampleCount - 1; i >= sampleCount - checkCount; i--) {
        shadow += shadowTap(cascade, uv + vogelDiskSample(i, sampleCount, phi) * texel * spread, reference);
    }
    if (shadow == 0.0 || shadow == f32(checkCount)) {
        shadow /= f32(checkCount);
    } else {
        for (var i = sampleCount - checkCount - 1; i >= 0; i--) {
            shadow += shadowTap(cascade, uv + vogelDiskSample(i, sampleCount, phi) * texel * spread, reference);
        }
        shadow /= f32(sampleCount);
    }
    return pow(clamp(shadow, 0.0, 1.0), 2.2);
}

// Where each cascade gives way to the next, in blocks ahead of the camera
// (GL's 16 and 44), and over how many blocks before that the two are mixed.
const shadowCascadeEnds = vec2f(16.0, 44.0);
const shadowCascadeBlend = vec2f(3.0, 8.0);

// How much of the sun reaches a point, 0-1. `viewDepth` is its distance
// ahead of the camera, which picks the cascade. Over the last blocks of a
// cascade it is mixed with the next: the two differ (a coarser map, a
// larger bias, so the far one has none of a building's own shadows), and a
// hard edge between them is a line that sweeps across the world, shadows
// appearing behind it, as the camera moves.
fn sunShadow(relative: vec3f, viewDepth: f32, dotLightNormal: f32, fragCoord: vec2f) -> f32 {
    let mode = u.lightParams.z;
    if (mode == 0) { return 1.0; }

    if (viewDepth >= shadowCascadeEnds.y) { return cascadeShadow(2u, relative, dotLightNormal, fragCoord, mode); }
    var cascade = 1u;
    var end = shadowCascadeEnds.y;
    var blend = shadowCascadeBlend.y;
    if (viewDepth < shadowCascadeEnds.x) {
        cascade = 0u;
        end = shadowCascadeEnds.x;
        blend = shadowCascadeBlend.x;
    }
    let shadow = cascadeShadow(cascade, relative, dotLightNormal, fragCoord, mode);
    let next = smoothstep(end - blend, end, viewDepth);
    if (next <= 0.0) { return shadow; }
    return mix(shadow, cascadeShadow(cascade + 1u, relative, dotLightNormal, fragCoord, mode), next);
}

// ---- The BRDF (defaultShader.frag, same names) ---------------------------

fn geometrySchlickGGX(NdotV: f32, roughness: f32) -> f32 {
    let k = roughness * roughness / 2.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 0.0000001);
}

fn geometrySmith(N: vec3f, V: vec3f, L: vec3f, roughness: f32) -> f32 {
    let NdotV = max(dot(N, V), 0.0);
    let NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotL, roughness) * geometrySchlickGGX(NdotV, roughness);
}

fn distributionGGX(N: vec3f, H: vec3f, roughness: f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let NdotH = max(dot(N, H), 0.0);
    var denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;
    return a2 / max(denom, 0.0000001);
}

fn fresnelSchlick(cosTheta: f32, F0: vec3f) -> vec3f {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

fn fSpecular(N: vec3f, H: vec3f, V: vec3f, L: vec3f, dotNVclamped: f32, roughness: f32, F: vec3f) -> vec3f {
    let NDF = distributionGGX(N, H, roughness);
    let G = geometrySmith(N, V, L, roughness);
    let denominator = 4.0 * dotNVclamped * max(dot(N, L), 0.0);
    return (NDF * G * F) / max(denominator, 0.001);
}

fn diffuseOrenNayar2(color: vec3f, roughness: f32, L: vec3f, V: vec3f, N: vec3f) -> vec3f {
    let a = roughness;
    let A = 1.0 / (PI + (PI / 2.0 - 2.0 / 3.0) * a);
    let B = PI / (PI + (PI / 2.0 - 2.0 / 3.0) * a);
    let s = dot(L, N) - dot(N, L) * dot(N, V);
    var t = 1.0;
    if (s > 0.0) { t = max(dot(N, L), dot(N, V)); }
    return color * (A + B * s / t);
}

// One light from direction L (toward the light), of color lightColor.
fn computePointLightSource(L: vec3f, metallic: f32, roughness: f32, lightColor: vec3f,
    V: vec3f, color: vec3f, N: vec3f, F0: vec3f) -> vec3f {
    let dotNVclamped = clamp(dot(N, V), 0.0, 0.99);
    let H = normalize(L + V);
    let F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    let specular = fSpecular(N, H, V, L, dotNVclamped, roughness, F);
    let kD = (vec3f(1.0) - F) * (1.0 - metallic); // what Fresnel leaves; metals have none
    let diffuse = diffuseOrenNayar2(color, roughness, L, V, N);
    let NdotL = max(dot(N, L), 0.0);
    return (kD * diffuse + specular) * lightColor * NdotL;
}

// ---- Normal maps ----------------------------------------------------------

// GL's NormalToRotation: a basis whose z is the face's normal, for turning a
// tangent-space normal map sample onto the face.
fn normalToRotation(normal: vec3f) -> mat3x3f {
    var tangent0 = cross(normal, vec3f(1.0, 0.0, 0.0));
    if (dot(tangent0, tangent0) < 0.001) { tangent0 = cross(normal, vec3f(0.0, 1.0, 0.0)); }
    tangent0 = normalize(tangent0);
    let tangent1 = normalize(cross(normal, tangent0));
    return mat3x3f(tangent0, tangent1, normal);
}

// GL's applyNormalMap, given the map's texel. Faces seen from behind (the
// transparent final pass draws both sides) turn the normal toward the eye.
fn applyNormalMap(faceNormal: vec3f, mapTexel: vec3f, frontFacing: bool) -> vec3f {
    var n = normalize(2.0 * mapTexel - 1.0);
    n = normalize(normalToRotation(faceNormal) * n);
    if (!frontFacing) { n = -n; }
    return n;
}

// ---- Ambient occlusion ----------------------------------------------------

// GL's texture-coordinate turns for the AO lookup.
fn rotate45(a: vec2f) -> vec2f {
    let b = a - vec2f(0.5);
    return vec2f(b.x + b.y, b.y - b.x) / sqrt(2.0) + vec2f(0.5);
}
fn rotate90(a: vec2f) -> vec2f { let b = a - vec2f(0.5); return vec2f(-b.y, b.x) + vec2f(0.5); }
fn rotate180(a: vec2f) -> vec2f { return vec2f(1.0) - a; }
fn rotateMinus90(a: vec2f) -> vec2f { let b = a - vec2f(0.5); return vec2f(b.y, -b.x) + vec2f(0.5); }
fn flipUV(a: vec2f) -> vec2f { return vec2f(a.y, a.x); }

// GL's getBlockAO, in two halves so the texture read stays in uniform
// control flow: aoCoordinate picks where to read for the face's AO shape
// (flag bits 4-7: none, the four sides, the four corners and their far
// variants, two full ones), aoFromTexel turns the texel into the factor.
fn aoShape(flags: u32) -> u32 {
    return (flags & 0xF0u) >> 4u;
}

fn aoCoordinate(flags: u32, uv: vec2f) -> vec2f {
    let shape = aoShape(flags);
    let cornerDarken1 = -vec2f(0.5, (sqrt(2.0) - 1.0) / 2.0) * 1.5;
    let r1 = rotate45(uv) + cornerDarken1;
    let r2 = rotate45(rotate90(uv)) + cornerDarken1;
    let r3 = rotate45(rotateMinus90(uv)) + cornerDarken1;
    let r4 = rotate45(flipUV(uv)) + cornerDarken1;
    let cornerDarken = -vec2f(0.5, (sqrt(2.0) - 1.0) / 2.0) * 3.0;
    let rez = array<vec2f, 14>(
        rotate90(uv), rotateMinus90(uv), rotate180(uv), uv,
        vec2f(r4.x, 1.0 - r4.y), vec2f(r1.x, 1.0 - r1.y), vec2f(r3.x, 1.0 - r3.y), vec2f(r2.x, 1.0 - r2.y),
        vec2f(r4.x, 1.0 - r4.y) + cornerDarken, vec2f(r1.x, 1.0 - r1.y) + cornerDarken,
        vec2f(r3.x, 1.0 - r3.y) + cornerDarken, vec2f(r2.x, 1.0 - r2.y) + cornerDarken,
        vec2f(0.5, 0.0), vec2f(0.5, 0.4),
    );
    let c = rez[clamp(i32(shape) - 1, 0, 13)];
    // ao.png is a wgpu2d texture, stored top row first, where gl2d stored it
    // bottom row first: v flipped, as for every 2D image read with GL uvs.
    return vec2f(c.x, 1.0 - c.y);
}

fn aoFromTexel(flags: u32, texel: f32) -> f32 {
    if (aoShape(flags) == 0u) { return 1.0; }
    return clamp(texel * 1.2, 0.70, 1.0) * 0.9;
}

// ---- Paint ----------------------------------------------------------------

fn rgb2hsv(c: vec3f) -> vec3f {
    let K = vec4f(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    let p = mix(vec4f(c.bg, K.wz), vec4f(c.gb, K.xy), step(c.b, c.g));
    let q = mix(vec4f(p.xyw, c.r), vec4f(c.r, p.yzx), step(p.x, c.r));
    let d = q.x - min(q.w, q.y);
    let e = 1.0e-10;
    return vec3f(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

fn hsv2rgb(c: vec3f) -> vec3f {
    let K = vec4f(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    let p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, vec3f(0.0), vec3f(1.0)), c.y);
}

// GL's paintGray, as GL has it: it treats the RGB color as HSV, so the
// result is GL's, not what the name says.
fn paintGray(colorIn: vec3f, newColor: vec3f) -> vec3f {
    var c = colorIn;
    c.y = 0.0;
    let colSqr = newColor.x * newColor.x;
    c.z = mix(c.z, mix(colSqr, c.z * colSqr, 0.6), 0.6);
    if (newColor.x == 0.0) { c.z = pow(c.z, 1.2); }
    return hsv2rgb(c);
}

fn paint(colorIn: vec3f, newColor: vec3f) -> vec3f {
    var hsv = rgb2hsv(colorIn);
    hsv.x = rgb2hsv(newColor).x;
    hsv.y = sqrt(min(hsv.y + 0.2, 1.0));
    let c = hsv2rgb(hsv);
    return mix(c * newColor, c, vec3f(0.4));
}

const paintColors = array<vec3f, 16>(
    vec3f(1.0, 1.0, 1.0),                         // white
    vec3f(0.75, 0.75, 0.75),                      // light gray
    vec3f(0.25, 0.25, 0.25),                      // dark gray
    vec3f(0.0, 0.0, 0.0),                         // black
    vec3f(0.36, 0.25, 0.20),                      // brown
    vec3f(1.0, 0.0, 0.0),                         // red
    vec3f(1.0, 0.5, 0.0),                         // orange
    vec3f(1.0, 1.0, 0.0),                         // yellow
    vec3f(81.0, 255.0, 0.0) / 255.0,              // lime
    vec3f(0.0, 0.1, 0.0),                         // green
    vec3f(0.0, 255.0, 195.0) / 255.0,             // turquoise
    vec3f(0.0, 1.0, 1.0),                         // cyan
    vec3f(0.0, 62.0, 176.0) / 255.0 * 1.2,        // blue
    vec3f(98.0, 0.0, 209.0) / 255.0,              // purple
    vec3f(1.0, 0.0, 1.0),                         // magenta
    vec3f(1.0, 0.5, 0.75),                        // pink
);

// The face's paint (the texture index's high 5 bits), 0 for none.
fn paintAlbedo(albedo: vec3f, colors: u32) -> vec3f {
    if (colors == 0u || colors > 16u) { return albedo; }
    if (colors - 1u <= 3u) { return paintGray(albedo, paintColors[colors - 1u]); }
    return paint(albedo, paintColors[colors - 1u]);
}

// ---- The face's light -----------------------------------------------------

// The baked light levels, as defaultShader.vert hands them on.
struct LightLevels {
    sky: i32,          // v_skyLight: sky light less what the time of day takes
    skyUnchanged: i32, // v_skyLightUnchanged: as baked
    block: i32,        // v_normalLight: from light blocks
};

fn lightLevels(packed: u32) -> LightLevels {
    var l: LightLevels;
    l.skyUnchanged = i32((packed & 0xF0u) >> 4u);
    l.block = i32(packed & 0xFu);
    l.sky = max(l.skyUnchanged - (15 - u.lightParams.x), 0);
    return l;
}

// The levels with the held light in (world.wgsl's heldLightLevel): it is a
// block light, so the brighter of it and the baked one counts, and daylight
// drowns it as it does a torch block's. For the ambient light only: the
// point lights test the baked level against their distance, and must not
// see this one.
fn withHeldLight(levels: LightLevels, relative: vec3f, normal: vec3f) -> LightLevels {
    var l = levels;
    l.block = max(l.block, heldLightLevel(relative, normal));
    return l;
}

// The top of defaultShader.frag's main: the ambient light, linear.
// `ambientMultiplier` is the orientation's fixed darkening (v_ambientMultiplier).
fn computeAmbient(levels: LightLevels, faceNormal: vec3f, ambientMultiplier: f32, inWater: bool, ao: f32,
    shadow: f32) -> vec3f {
    let baseAmbient = 0.20 + u.sunDirection.w;
    let multiplier = 0.70;
    let shadowStrength = 5.0;

    var newSkyLight = f32(levels.sky);
    if (levels.sky > 6) {
        newSkyLight -= shadowStrength;
        newSkyLight += shadowStrength * shadow * clamp(dot(u.sunDirection.xyz, faceNormal), 0.0, 1.0);
    }
    let newAmbient = max(newSkyLight, f32(levels.block)) / 15.0 * ambientMultiplier;
    var ambient = multiplier * vec3f(min(toLinear1(newAmbient * (1.0 - baseAmbient) + baseAmbient), 1.0));

    if (inWater) { ambient *= vec3f(0.38, 0.4, 0.42); }
    if (levels.sky > levels.block || levels.block == 0) {
        // The sky's color by day or night where it reaches; where it
        // doesn't (a cave), no brighter than a dim grey whatever the time,
        // so that the day's white doesn't light it. Between, by how much
        // sky reaches, so a cave mouth fades into the dark.
        let reach = f32(levels.skyUnchanged) / 15.0;
        let unlit = min(u.ambientColor.rgb, vec3f(0.35));
        ambient *= mix(unlit, u.ambientColor.rgb, reach);
    } else {
        ambient *= vec3f(255.0, 237.0, 214.0) / 255.0; // torch light, slightly warmer
    }
    return ambient * ao * 1.2;
}

fn atenuationFunction(dist: f32) -> f32 {
    let maxDist = 15.0;
    return max(0.0, (maxDist - dist) / (dist * dist * 0.5 + maxDist));
}

// The light blocks' contribution. A light counts if it is within 15 blocks
// and its light could reach this face's level by block steps (GL's test on
// the Manhattan distance), so lights behind walls stay dark.
fn pointLights(levels: LightLevels, blockPos: vec3i, inBlock: vec3f, metallic: f32, roughness: f32,
    V: vec3f, albedo: vec3f, N: vec3f, F0: vec3f, caustics: vec3f) -> vec3f {
    var color = vec3f(0.0);
    if (levels.block == 0) { return color; }
    let count = u32(max(u.lightParams.y, 0));
    for (var i = 0u; i < count; i++) {
        let light = lights[i].xyz;
        // GL's compute(light, 0, fragmentPositionI, fragmentPositionF):
        // the integer difference first, then the fraction.
        var L = vec3f(light - blockPos) - inBlock;
        let lightDist = length(L);
        if (lightDist > 15.0) { continue; }
        let manhattan = f32(dot(abs(blockPos - light), vec3i(1)));
        if ((15.0 - manhattan) + 0.1 > f32(levels.block)) { continue; }
        L = normalize(L);
        color += computePointLightSource(L, metallic, roughness, caustics, V, albedo, N, F0)
            * atenuationFunction(lightDist) * ((f32(levels.block) / 15.0) + 1.0) / 2.0;
    }
    return color;
}

// The sun (or moon) on a face that sees the sky. `caustics` tints the
// light, as on the point lights (1 out of water).
fn sunLight(levels: LightLevels, metallic: f32, roughness: f32, V: vec3f, albedo: vec3f, N: vec3f,
    F0: vec3f, ao: f32, shadow: f32, caustics: vec3f) -> vec3f {
    if (levels.skyUnchanged <= 3) { return vec3f(0.0); }
    var color = u.sunLightColor.rgb * 1.9;
    // Less where little sky light reaches, and a little less at full light.
    color *= clamp(1.0 - (f32(15 - levels.skyUnchanged) / 9.0), 0.0, 1.0);
    let ambientInt = max(levels.sky, levels.block);
    let decrease = clamp(f32(ambientInt - 10) / 5.0, 0.0, 1.0);
    color *= mix(1.0, 0.7, decrease);
    let L = u.sunDirection.xyz;
    if (dot(L, vec3f(0.0, 1.0, 0.0)) <= -0.2) { return vec3f(0.0); }
    return computePointLightSource(L, metallic, roughness, color * caustics, V, albedo, N, F0) * shadow * sqrt(ao);
}

// The material map: roughness from red, metallic from green, emission blue.
struct Material {
    roughness: f32,
    metallic: f32,
    emissive: f32,
};

fn material(texel: vec3f) -> Material {
    var m: Material;
    m.roughness = clamp(pow(1.0 - texel.r, 2.0), 0.09, 0.99);
    m.metallic = clamp(pow(texel.g, 0.5), 0.0, 0.98);
    m.emissive = texel.b;
    return m;
}
