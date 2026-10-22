#version 440
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 m0;
layout(location = 3) in vec4 m1;
layout(location = 4) in vec4 m2;
layout(location = 5) in vec4 m3;
layout(location = 6) in vec4 colour;
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec4 v_colour;
layout(location = 2) out vec3 v_world;
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
    mat4 model = mat4(m0, m1, m2, m3);
    vec4 w = model * vec4(position, 1.0);
    // Box models are T*R*S, so the scaled axis normal still points the right way once normalised.
    v_normal = normalize(mat3(model) * normal);
    v_colour = colour;
    v_world = w.xyz;
    gl_Position = view_proj * w;
}
