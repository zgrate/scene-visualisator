#version 440
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_colour;
layout(location = 2) in vec3 v_world;
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
void main() {
    vec3 n = normalize(v_normal);
    float diffuse = max(dot(n, normalize(light.xyz)), 0.0);
    float fill = max(dot(n, normalize(eye.xyz - v_world)), 0.0);
    vec3 lit = v_colour.rgb * (0.28 + 0.55 * diffuse + 0.25 * fill);
    frag = vec4(mix(lit, v_colour.rgb, clamp(v_colour.a - 1.0, 0.0, 1.0)), 1.0); // alpha 2: emissive
}
