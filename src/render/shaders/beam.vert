#version 440
layout(location = 0) in vec3 position; // unit cone: apex at the origin, base radius 1 at z = 1
layout(location = 1) in vec4 origin_len;
layout(location = 2) in vec4 dir_tan;
layout(location = 3) in vec4 colour;
layout(location = 0) out vec3 v_world;
layout(location = 1) flat out vec4 v_origin_len;
layout(location = 2) flat out vec4 v_dir_tan;
layout(location = 3) flat out vec4 v_colour;
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
void main() {
    vec3 d = dir_tan.xyz;
    vec3 u = normalize(cross(d, abs(d.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 v = cross(d, u);
    float s = position.z * origin_len.w;
    float r = (lens + s * dir_tan.w) * 1.15; // slack so the fragment profile, not the 24-gon, sets the edge
    v_world = origin_len.xyz + d * s + (position.x * u + position.y * v) * r;
    v_origin_len = origin_len;
    v_dir_tan = dir_tan;
    v_colour = colour;
    gl_Position = view_proj * vec4(v_world, 1.0);
}
