#version 440
layout(location = 0) out vec2 v_uv;
layout(std140, binding = 0) uniform Post {
    vec4 texel;  // xy: 1 / source size, z: 1 to flip v, w: 1 on the first down pass (threshold and Karis average)
    vec4 params; // x: bloom strength, y: time in seconds, z: bloom threshold
};
// One triangle covering the target. v is flipped where NDC and framebuffer disagree on which way is up (D3D, Metal).
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = texel.z > 0.5 ? vec2(p.x, 1.0 - p.y) : p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
