#version 440
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 0) out vec3 v_world;
layout(std140, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 eye;        // w: time in seconds
    vec4 light;      // xyz: direction toward the light, w: 1 when drawing into the HDR target
    vec4 floor_info; // venue half width, half depth, haze, floor spot count
    vec4 venue;
    vec4 spot_pos[32]; // x, z centre, radius
    vec4 spot_col[32];
};
void main() {
    v_world = vec3(position.x * 400.0, 0.0, position.y * 400.0);
    gl_Position = view_proj * vec4(v_world, 1.0);
}
