// What every world shader shares (milestone 12): the uniforms and the color
// helpers. Included with `#include "world.wgsl"` (wgpu2d's shader loader
// expands it; WGSL itself has no includes). Must match wgpuWorld.cpp's
// Uniforms.

struct Uniforms {
    // perspectiveRH_ZO times Camera::getViewMatrix (rotation only).
    viewProjection: mat4x4f,
    // The camera position split as in Camera::decomposePosition, the
    // u_positionInt / u_positionFloat pair. w unused.
    positionInt: vec4i,
    positionFloat: vec4f,
    // GL's u_waterColor, linear.
    waterColor: vec4f,
    // x near plane, y far plane: to turn depth back into distance. z the
    // crack frame, 0-10, for fs_decal (written when a decal is drawn).
    // w the exposure the composite applies (adaptiveExposure.currentExposure).
    planes: vec4f,
    // Camera::getViewMatrix: camera-relative positions to view space, for
    // the G-buffer's positions (GL's u_view).
    view: mat4x4f,
    // The direction toward the sun (or the moon at night: GL's
    // mainLightPosition); w GL's u_baseAmbientExtra.
    sunDirection: vec4f,
    // GL's u_sunLightColor and u_ambientColor, from the time of day.
    sunLightColor: vec4f,
    ambientColor: vec4f,
    // x GL's u_skyLightIntensity (0-15, the time of day's sky light), y the
    // point lights in `lights` (0 with lights off), z the shadows setting
    // (0 off, 1 hard, 2 soft; milestone 13), w 1 when the SSR setting is on
    // (milestone 17; GL's c_SSR).
    lightParams: vec4i,
    // The sun's shadow cascades (GL's u_cascadedShadowMatrix), nearest
    // first: camera-relative positions to each shadow map's clip space,
    // depth 0..1. Valid while lightParams.z is not 0.
    shadowMatrices: array<mat4x4f, 3>,
    // Milestone 14's fog (GL's ShadingSettings): x u_fogDistance, where the
    // far fog closes (the view distance less a chunk), y u_fogCloseGradient
    // (the fogGradient setting; 0 disables the near fog), z 1 underwater
    // (GL fogs only above water), w unused.
    fogParams: vec4f,
    // Milestone 17: the camera's projection (perspectiveRH_ZO; GL's
    // u_cameraProjection, whose x, y and w rows are the same), view space
    // to clip space, for the reflections' ray march.
    projection: mat4x4f,
    // Milestone 18, under water (fogParams.z): rgb GL's u_underWaterColor
    // (as GL uses it, unconverted), w u_underwaterDarkenStrength; and x
    // u_underwaterDarkenDistance, y u_fogGradientUnderWater.
    underWaterColor: vec4f,
    underWaterParams: vec4f,
    // 18-parity: x seconds since start (GL's u_timeGrass, on Windows wall
    // time), for the grass, leaves and waves; y GL's u_waterMove (0..20),
    // for the water's textures.
    animation: vec4f,
};
@group(0) @binding(0) var<uniform> u: Uniforms;

// GL's linear <-> sRGB (defaultShader.frag's toLinearSRGB, and its inverse).
fn toLinear(c: vec3f) -> vec3f {
    let low = c / 12.92;
    let high = pow((c + 0.055) / 1.055, vec3f(2.4));
    return select(high, low, c < vec3f(0.04045));
}

fn toLinear1(c: f32) -> f32 {
    return toLinear(vec3f(c)).x;
}

fn fromLinear(c: vec3f) -> vec3f {
    let low = c * 12.92;
    let high = 1.055 * pow(c, vec3f(1.0 / 2.4)) - 0.055;
    return select(high, low, c < vec3f(0.0031308));
}

// defaultShader.frag's fromFloatTouShort: [-1, 1] to [0, 65536], clamped to
// 65535 (GL's 65536 for a component of exactly 1 does not fit 16 bits).
fn packNormal(n: vec3f) -> vec4u {
    return vec4u(min(vec3u((n + 1.0) * 0.5 * 65536.0), vec3u(65535u)), 0u);
}
