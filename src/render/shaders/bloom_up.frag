#version 440
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag;
layout(std140, binding = 0) uniform Post {
    vec4 texel;  // xy: 1 / source size, z: 1 to flip v, w: 1 on the first down pass (threshold and Karis average)
    vec4 params; // x: bloom strength, y: time in seconds, z: bloom threshold
};
layout(binding = 1) uniform sampler2D source;
vec3 tap(float x, float y) { return texture(source, v_uv + texel.xy * vec2(x, y)).rgb; }
// 3x3 tent, added onto the next larger mip by the pipeline's One/One blend.
void main() {
    vec3 s = tap(0, 0) * 4.0 + (tap(-1, 0) + tap(1, 0) + tap(0, -1) + tap(0, 1)) * 2.0 +
             tap(-1, -1) + tap(1, -1) + tap(-1, 1) + tap(1, 1);
    frag = vec4(s / 16.0, 1.0);
}
