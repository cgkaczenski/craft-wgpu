// Milestone 17: ambient occlusion (Renderer::renderFromBakedData's "hbao";
// postProcess/hbao.frag and applyHBAO.frag). GL names it HBAO, but its live
// code is SSAO (the horizon-based version is commented out there):
//
//   fs_hbao   at half the surface's size, into an r8unorm target (GL's
//             fboHBAO is GL_RED): 20 of 32 hemisphere samples around the
//             normal, 0.5 blocks out, each projected to the screen and
//             counted when the G-buffer's position there is in front of it.
//   fs_apply  a 4x4 box of that, faded out toward the view distance, as
//             black with that alpha over the HDR color (GL's SrcAlpha,
//             OneMinusSrcAlpha blend), 1.45 times as strong.
//
// Positions are read through the linear sampler as GL's texture() reads
// its GL_LINEAR targets (a half-size pixel's center falls between four of
// them). The normals are integers, which nothing filters: textureLoad of
// the texel under the coordinate, what GL's drivers that allow the read give.

struct HbaoUniforms {
    // Camera::getViewMatrix (rotation only): world normals to view space.
    view: mat4x4f,
    // The camera's projection (x, y and w rows as GL's u_projection).
    projection: mat4x4f,
    // x u_viewDistance ((squareSize / 2) * 16), yzw unused.
    params: vec4f,
};
@group(0) @binding(0) var<uniform> hbao: HbaoUniforms;
@group(0) @binding(1) var positionTexture: texture_2d<f32>; // view space; sky 0
@group(0) @binding(2) var normalTexture: texture_2d<u32>;   // packed world normals
@group(0) @binding(3) var aoTexture: texture_2d<f32>;       // fs_hbao's output
@group(0) @binding(4) var linearSampler: sampler;           // linear, clamped

struct HbaoOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
};

// drawQuads.vert's quad as one triangle; uv 0..1 across the target, v down.
@vertex
fn vs_fullscreen(@builtin(vertex_index) index: u32) -> HbaoOutput {
    let x = f32((index << 1u) & 2u) * 2.0 - 1.0;
    let y = f32(index & 2u) * 2.0 - 1.0;
    var out: HbaoOutput;
    out.position = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f(0.5 + 0.5 * x, 0.5 - 0.5 * y);
    return out;
}

// GL's 32 kernel samples, z up the normal, as a switch rather than a
// constant array: naga rejected a literal array in bloom.wgsl
// (ExpressionAlreadyInScope), and a switch cannot trip that.
fn kernelSample(i: i32) -> vec3f {
    switch i {
        case 0: { return vec3f(0.40380573, 0.62488124, 0.66817989); }
        case 1: { return vec3f(0.94561738, -0.19045641, 0.26369323); }
        case 2: { return vec3f(-0.36234207, 0.53053288, 0.76631788); }
        case 3: { return vec3f(0.65091370, -0.18482486, 0.73630912); }
        case 4: { return vec3f(-0.94016065, 0.04093710, 0.33826337); }
        case 5: { return vec3f(0.77078830, 0.48419549, 0.41405329); }
        case 6: { return vec3f(0.04710537, 0.80822038, 0.58699310); }
        case 7: { return vec3f(0.64122709, -0.59576237, 0.48362695); }
        case 8: { return vec3f(0.84031517, -0.51174415, 0.17885284); }
        case 9: { return vec3f(0.55245552, -0.65680562, 0.51322439); }
        case 10: { return vec3f(-0.19991502, 0.97309764, 0.11452063); }
        case 11: { return vec3f(-0.11788207, -0.52548180, 0.84259878); }
        case 12: { return vec3f(0.54185599, 0.79889750, 0.26106486); }
        case 13: { return vec3f(-0.95743544, -0.10821882, 0.26759310); }
        case 14: { return vec3f(-0.63843815, -0.00817195, 0.76962975); }
        case 15: { return vec3f(0.74838251, 0.51509018, 0.41785849); }
        case 16: { return vec3f(-0.12723010, 0.97333021, 0.19089472); }
        case 17: { return vec3f(0.66155893, 0.66285931, 0.35065272); }
        case 18: { return vec3f(0.77054173, -0.49873825, 0.39689495); }
        case 19: { return vec3f(-0.78277062, 0.18149567, 0.59525580); }
        case 20: { return vec3f(0.47278898, -0.02632285, 0.88078243); }
        case 21: { return vec3f(0.52695586, -0.82310353, 0.21170286); }
        case 22: { return vec3f(0.58856750, 0.39199484, 0.70705611); }
        case 23: { return vec3f(-0.51261935, 0.69005005, 0.51093281); }
        case 24: { return vec3f(-0.82765341, 0.56071328, 0.02429934); }
        case 25: { return vec3f(0.64003532, -0.33541669, 0.69126727); }
        case 26: { return vec3f(0.69955083, 0.20104806, 0.68571737); }
        case 27: { return vec3f(-0.72598904, 0.66346620, 0.18097654); }
        case 28: { return vec3f(0.04473761, -0.37769375, 0.92484916); }
        case 29: { return vec3f(0.41045231, 0.33813962, 0.84687101); }
        case 30: { return vec3f(-0.63402933, -0.67922671, 0.36967268); }
        default: { return vec3f(-0.12723010, 0.97333021, 0.19089472); }
    }
}

// GL's hash123: a random unit vector from a position's xz.
fn hash123(p: vec2f) -> vec3f {
    var p3 = fract(vec3f(p.x, p.y, p.x) * vec3f(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 19.19);
    var rez = fract((p3.xx + p3.yz) * p3.zy);
    rez = rez * 2.0 - 1.0;
    return normalize(vec3f(rez.x, rez.y, p3.x));
}

// fromuShortToFloat: [0, 65536] back to a unit vector.
fn unpackNormal(n: vec3u) -> vec3f {
    return normalize(vec3f(n) / 65536.0 * 2.0 - 1.0);
}

// The full-size texel under a 0..1 coordinate (v down).
fn texelOf(uv: vec2f, size: vec2u) -> vec2i {
    return clamp(vec2i(uv * vec2f(size)), vec2i(0), vec2i(size) - 1);
}

const kernelSize = 32;
const samplesTestSize = 20;
const radius = 0.5;
const bias = 0.2;

@fragment
fn fs_hbao(in: HbaoOutput) -> @location(0) vec4f {
    let fragPos = textureSampleLevel(positionTexture, linearSampler, in.uv, 0.0).xyz;
    // The sky: nothing drawn, no occlusion.
    if (fragPos.z == 0.0) { return vec4f(0.0, 0.0, 0.0, 1.0); }

    let packed = textureLoad(normalTexture, texelOf(in.uv, textureDimensions(normalTexture)), 0).xyz;
    let normal = normalize((hbao.view * vec4f(unpackNormal(packed), 0.0)).xyz);

    let randomVec = hash123(fragPos.xz);
    let tangent = normalize(randomVec - normal * dot(randomVec, normal));
    let bitangent = cross(normal, tangent);
    let TBN = mat3x3f(tangent, bitangent, normal);

    var occlusion = 0.0;
    let begin = i32(f32(kernelSize - samplesTestSize) * abs(randomVec.x));
    for (var i = begin; i < begin + samplesTestSize; i++) {
        let samplePos = fragPos + TBN * kernelSample(i) * radius;
        let clip = hbao.projection * vec4f(samplePos, 1.0);
        let ndc = clip.xy / clip.w;
        // GL's offset.xy * 0.5 + 0.5, with v flipped for top-first rows.
        let sampleUv = vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        let sampleDepth = textureSampleLevel(positionTexture, linearSampler, sampleUv, 0.0).z;
        let rangeCheck = smoothstep(0.0, 1.0, radius / abs(fragPos.z - sampleDepth));
        occlusion += select(0.0, 1.0, sampleDepth >= samplePos.z + bias) * rangeCheck;
    }
    occlusion = pow(occlusion / f32(kernelSize), 0.9);
    return vec4f(occlusion, 0.0, 0.0, 1.0);
}

// applyHBAO.frag's fog: full strength until 90% of the view distance's
// curve, then quickly gone.
fn aoFog(dist: f32) -> f32 {
    let rez = exp(-pow(dist * (1.0 / hbao.params.x), 16.0));
    if (rez > 0.9) { return 1.0; }
    return pow(rez, 16.0);
}

@fragment
fn fs_apply(in: HbaoOutput) -> @location(0) vec4f {
    let dist = -textureSampleLevel(positionTexture, linearSampler, in.uv, 0.0).z;
    // GL's offsets -2..1 run up the screen; v runs down here, so y is
    // negated to cover the same 4x4 box.
    let texelSize = 1.0 / vec2f(textureDimensions(aoTexture));
    var aoValue = 0.0;
    for (var y = -2; y < 2; y++) {
        for (var x = -2; x < 2; x++) {
            aoValue += textureSampleLevel(aoTexture, linearSampler, in.uv + vec2f(f32(x), -f32(y)) * texelSize, 0.0).r;
        }
    }
    aoValue /= 16.0;
    return vec4f(0.0, 0.0, 0.0, aoValue * 1.45 * aoFog(dist));
}
