#version 440
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag;
layout(std140, binding = 0) uniform Post {
    vec4 texel;  // xy: 1 / source size, z: 1 to flip v, w: 1 on the first down pass (threshold and Karis average)
    vec4 params; // x: bloom strength, y: time in seconds, z: bloom threshold
};
layout(binding = 1) uniform sampler2D source;
vec3 tap(float x, float y) { return texture(source, v_uv + texel.xy * vec2(x, y)).rgb; }
float karis(vec3 c) { return 1.0 / (1.0 + max(c.r, max(c.g, c.b))); } // tames lone hot pixels, so bloom does not flicker
// 13-tap downsample (Jimenez, "Next generation post processing in Call of Duty").
void main() {
    vec3 a = tap(-2, -2), b = tap(0, -2), c = tap(2, -2), d = tap(-2, 0), e = tap(0, 0), f = tap(2, 0);
    vec3 g = tap(-2, 2), h = tap(0, 2), i = tap(2, 2), j = tap(-1, -1), k = tap(1, -1), l = tap(-1, 1), m = tap(1, 1);
    vec3 s;
    if (texel.w > 0.5) { // Karis average of the five 2x2 groups, then a soft-knee threshold
        vec3 grp[5] = vec3[5]((j + k + l + m) * 0.25, (a + b + d + e) * 0.25, (b + c + e + f) * 0.25,
                              (d + e + g + h) * 0.25, (e + f + h + i) * 0.25);
        float total = 0.0;
        s = vec3(0.0);
        for (int n = 0; n < 5; ++n) {
            float w = (n == 0 ? 0.5 : 0.125) * karis(grp[n]);
            s += grp[n] * w;
            total += w;
        }
        s /= total;
        float bright = max(s.r, max(s.g, s.b)), knee = 0.5 * params.z;
        float q = clamp(bright - params.z + knee, 0.0, 2.0 * knee);
        s *= max(q * q / (4.0 * knee + 1e-4), bright - params.z) / max(bright, 1e-4);
    } else {
        s = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    frag = vec4(max(s, vec3(0.0)), 1.0);
}
