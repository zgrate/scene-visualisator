#version 440
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 m0;
layout(location = 3) in vec4 m1;
layout(location = 4) in vec4 m2;
layout(location = 5) in vec4 m3;
layout(location = 6) in vec4 slice;
layout(location = 7) in vec4 grid;
layout(location = 8) in vec4 params;
layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out vec4 v_slice;
layout(location = 2) flat out vec4 v_grid;
layout(location = 3) flat out vec4 v_params;
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
    v_uv = vec2(position.x + 0.5, 0.5 - position.y);
    v_slice = slice;
    v_grid = grid;
    v_params = params;
    gl_Position = view_proj * (mat4(m0, m1, m2, m3) * vec4(position, 1.0));
}
