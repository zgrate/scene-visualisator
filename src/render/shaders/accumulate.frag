#version 440
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag;
layout(std140, binding = 0) uniform Post {
    vec4 texel;  // z: 1 to flip v
    vec4 params; // x: this sub-frame's weight
};
layout(binding = 1) uniform sampler2D source;
// One bilinear tap: from a source at exactly 2x it lands on a 2x2 texel corner and averages the four (a box filter).
void main() { frag = texture(source, v_uv) * params.x; }
