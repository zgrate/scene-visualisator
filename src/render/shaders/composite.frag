#version 440
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag;
layout(std140, binding = 0) uniform Post {
    vec4 texel;  // xy: 1 / source size, z: 1 to flip v, w: 1 on the first down pass (threshold and Karis average)
    vec4 params; // x: bloom strength, y: time in seconds, z: bloom threshold, w: 1 + haze in cinematic, else 0
};
layout(binding = 1) uniform sampler2D source;
layout(binding = 2) uniform sampler2D bloom;
const float knee = 0.8;
vec3 aces(vec3 x) { return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); } // Narkowicz fit
void main() {
    vec2 d = v_uv - 0.5;
    float n = fract(sin(dot(gl_FragCoord.xy + params.y, vec2(12.9898, 78.233))) * 43758.5453);
    if (params.w > 0.5) { // cinematic: lens fringe at the edges, a haze veil from the bloom, filmic tone map
        vec2 fringe = d * dot(d, d) * 0.012;
        vec3 s = vec3(texture(source, v_uv + fringe).r, texture(source, v_uv).g, texture(source, v_uv - fringe).b);
        vec3 c = aces((s + texture(bloom, v_uv).rgb * (params.x + 0.6 * (params.w - 1.0))) * 1.1);
        c *= 1.0 - 0.2 * smoothstep(0.15, 0.75, dot(d, d) * 2.0);
        frag = vec4(c + (n - 0.5) / 255.0, 1.0);
        return;
    }
    vec3 c = texture(source, v_uv).rgb + texture(bloom, v_uv).rgb * params.x;
    // Highlight shoulder only: below the knee the image is untouched (Theme colours stay exact); above it the
    // brightest channel rolls off toward 1 and the colour bleaches toward white, like a hot light on camera.
    float m = max(c.r, max(c.g, c.b));
    if (m > knee) {
        float x = m - knee;
        float rolled = knee + (1.0 - knee) * (1.0 - exp(-x / (1.0 - knee)));
        c = mix(c * (rolled / m), vec3(rolled), 1.0 - 1.0 / (1.0 + 0.2 * x));
    }
    c *= 1.0 - 0.12 * smoothstep(0.2, 0.75, dot(d, d) * 2.0); // gentle vignette
    frag = vec4(c + (n - 0.5) / 255.0, 1.0); // dither against banding in dark haze
}
