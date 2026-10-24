#version 440
layout(location = 0) in vec3 v_world;
layout(location = 1) flat in vec4 v_origin_len;
layout(location = 2) flat in vec4 v_dir_tan;
layout(location = 3) flat in vec4 v_colour;
layout(location = 0) out vec4 frag;
layout(std140, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 eye;        // w: time in seconds
    vec4 light;      // xyz: direction toward the light
    vec4 floor_info; // venue half width, half depth, haze, floor spot count
    vec4 venue;
    vec4 spot_pos[32]; // x, z centre, radius
    vec4 spot_col[32];
};
const float lens = 0.06;
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
    float hot = 1.0 + 2.0 * exp(-s * 2.0); // bright near the lens
    // Clamped for the screen blend, which needs a source within 0..1.
    frag = vec4(min(v_colour.rgb * profile * spread * falloff * hot * (0.15 + floor_info.z), vec3(1.0)), 0.0);
}
