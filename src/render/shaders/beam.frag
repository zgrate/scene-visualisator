#version 440
layout(location = 0) in vec3 v_world;
layout(location = 1) flat in vec4 v_origin_len;
layout(location = 2) flat in vec4 v_dir_tan;
layout(location = 3) flat in vec4 v_colour;
layout(location = 0) out vec4 frag;
layout(std140, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 eye;        // w: time in seconds
    vec4 light;      // xyz: direction toward the light, w: 1 when drawing into the HDR target
    vec4 floor_info; // venue half width, half depth, haze, floor spot count
    vec4 venue;
    vec4 spot_pos[32]; // x, z centre, radius
    vec4 spot_col[32];
};
const float lens = 0.06;
float hash(vec3 p) {
    p = fract(p * 0.3183099 + 0.1) * 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float noise(vec3 x) { // value noise in 0..1
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash(i), hash(i + vec3(1, 0, 0)), f.x), mix(hash(i + vec3(0, 1, 0)), hash(i + vec3(1, 1, 0)), f.x),
                   f.y),
               mix(mix(hash(i + vec3(0, 0, 1)), hash(i + vec3(1, 0, 1)), f.x),
                   mix(hash(i + vec3(0, 1, 1)), hash(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}
// Analytic single-scattering look: find where the view ray passes closest to the beam axis and use the
// radial profile there. Drawn on back faces only, additively, so it also works with the camera inside a beam.
void main() {
    vec3 ro = eye.xyz;
    vec3 to_frag = v_world - ro;
    float t_back = length(to_frag);
    vec3 rd = to_frag / t_back;
    vec3 o = v_origin_len.xyz, d = v_dir_tan.xyz;
    float len = v_origin_len.w;
    vec3 w0 = ro - o;
    float b = dot(rd, d), e = dot(d, w0), dd = dot(rd, w0);
    float denom = max(1.0 - b * b, 1e-5);
    float s = clamp((e - b * dd) / denom, 0.0, len);
    float t = clamp(dot(o + d * s - ro, rd), 0.0, t_back);
    float r = length(ro + rd * t - (o + d * s));
    float R = lens + s * v_dir_tan.w;
    float profile = sqrt(max(1.0 - (r * r) / (R * R), 0.0));
    float spread = 1.0 / (1.0 + 0.6 * R);
    float falloff = mix(1.0, 0.45, s / len) * (1.0 - smoothstep(len * 0.97, len, s));
    vec3 c = v_colour.rgb * profile * spread * falloff * (0.15 + floor_info.z);
    if (light.w < 0.5) { // screen blend into 8 bits: needs a source within 0..1
        frag = vec4(min(c * (1.0 + 2.0 * exp(-s * 2.0)), vec3(1.0)), 0.0);
        return;
    }
    vec3 drift = vec3(0.05, 0.12, 0.03) * eye.w;
    float thick = clamp(0.5 + floor_info.z, 0.0, 1.0);
    if (light.w > 1.5) {
        // Cinematic: march the stretch of the view ray inside the cone. In-scatter per volume follows the flux per
        // area (1 / R²), smoke is the density, the haze absorbs a little along the beam, and a Henyey-Greenstein
        // phase makes a beam aimed at the camera brighter. Jittered per pixel; the offline render's sub-frames and
        // supersampling average the grain away.
        float half_chord = sqrt(max(R * R - r * r, 0.0)) / max(sqrt(denom), 0.05);
        float t0 = max(t - half_chord, 0.0), t1 = min(t + half_chord, t_back);
        const int steps = 24;
        float ds = max(t1 - t0, 0.0) / float(steps);
        float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)) + eye.w) * 43758.5453);
        float acc = 0.0;
        for (int i = 0; i < steps; ++i) {
            vec3 p = ro + rd * (t0 + (float(i) + jitter) * ds);
            float sp = dot(p - o, d);
            if (sp < 0.0 || sp > len)
                continue;
            float rp = lens + sp * v_dir_tan.w;
            float q = length(p - o - d * sp) / rp;
            if (q >= 1.0)
                continue;
            float smoke = 0.65 * noise(p * 1.3 + drift) + 0.35 * noise(p * 3.1 - drift * 1.7);
            float density = mix(1.0, 0.15 + 1.9 * smoke * smoke, thick);
            acc += (1.0 - q * q) * density * exp(-0.04 * sp) * (1.0 - smoothstep(len * 0.97, len, sp)) * ds / (rp * rp);
        }
        const float g = 0.45;
        float phase = pow((1.0 + g * g) / (1.0 + g * g - 2.0 * g * dot(d, -rd)), 1.5); // 1 at 90°
        vec3 beam = v_colour.rgb * acc * phase * (0.15 + floor_info.z) * 0.5;
        // The lens: blinding when the fixture points at the camera, with a soft halo for the bloom to spread.
        float along = dot(o - ro, rd);
        float r0 = along > 0.0 ? length(ro + rd * along - o) : 1e3;
        float facing = pow(max(dot(d, -rd), 0.0), 8.0);
        vec3 glow = v_colour.rgb * facing * (6.0 * exp(-r0 * r0 / (lens * lens)) + 0.3 * exp(-r0 / 0.25));
        frag = vec4(beam + glow, 0.0);
        return;
    }
    // HDR, added up: a hot core at the lens for the bloom, and drifting haze where the view ray meets the beam.
    vec3 p = ro + rd * t;
    float smoke = 0.65 * noise(p * 1.3 + drift) + 0.35 * noise(p * 3.1 - drift * 1.7);
    float hot = 1.0 + 2.5 * exp(-s * 2.5) + 10.0 * exp(-s * 10.0) * profile * profile;
    frag = vec4(c * hot * mix(1.0, 0.15 + 1.9 * smoke * smoke, thick), 0.0);
}
