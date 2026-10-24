#version 440
layout(location = 0) in vec3 v_world;
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
float grid_line(vec2 p, float spacing, float width) {
    vec2 c = p / spacing;
    vec2 g = abs(fract(c - 0.5) - 0.5) / fwidth(c);
    return 1.0 - min(min(g.x, g.y) / width, 1.0);
}
void main() {
    vec2 p = v_world.xz;
    vec2 d = abs(p) - floor_info.xy;
    float edge = max(d.x, d.y);
    vec3 c = edge < 0.0 ? vec3(0.062, 0.067, 0.082) : vec3(0.043, 0.047, 0.063);
    float fade = 1.0 - smoothstep(25.0, 110.0, length(v_world - eye.xyz));
    vec3 line = vec3(0.149, 0.165, 0.2); // Theme grid token
    c = mix(c, line, grid_line(p, 1.0, 1.0) * 0.7 * fade);
    c = mix(c, line * 1.5, grid_line(p, 5.0, 1.4) * fade);
    float outline = 1.0 - min(abs(edge) / fwidth(edge) / 1.5, 1.0);
    c = mix(c, vec3(0.36, 0.38, 0.44), outline * fade);
    int n = int(floor_info.w);
    vec3 pools = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        float r = length(p - spot_pos[i].xy) / max(spot_pos[i].z, 0.01);
        pools += spot_col[i].rgb * (1.0 - smoothstep(0.75, 1.0, r)) * (0.35 + 0.25 * (1.0 - r));
    }
    frag = vec4(c + 1.0 - exp(-1.5 * pools), 1.0); // soft limit: overlapping pools stop short of white
}
