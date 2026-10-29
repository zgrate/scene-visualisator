#version 440
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec3 v_world;
layout(location = 0) out vec4 frag;
layout(std140, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 eye;        // w: time in seconds
    vec4 light;      // xyz: direction toward the light, w: 1 when drawing into the HDR target
    vec4 floor_info; // venue half width, half depth, haze, floor spot count
    vec4 venue;
    vec4 spot_pos[32]; // x, z centre, radius
    vec4 spot_col[32];
};
layout(binding = 1) uniform sampler2D composition;
#include "lights.glsl"
// Cinematic: the glossy floor's reflection of the nearest LED wall along the mirrored view ray, blurred by distance
// through the composition's mips and weighted by Fresnel. Same slice mapping as wall.frag, without the LED cells.
vec3 wall_reflection(vec3 p, vec3 v) {
    vec3 rd = reflect(-v, vec3(0.0, 1.0, 0.0));
    float best = 1e9;
    vec3 col = vec3(0.0);
    for (int i = 0; i < int(counts.z); ++i) {
        vec3 c = wall_c[i].xyz, ax = wall_x[i].xyz, ay = wall_y[i].xyz;
        vec3 nrm = cross(ax, ay);
        float dn = dot(rd, nrm);
        if (dn >= 0.0) // the back of the wall
            continue;
        float t = dot(c - p, nrm) / dn;
        if (t <= 0.0 || t >= best)
            continue;
        vec3 h = p + rd * t - c;
        vec2 q = vec2(dot(h, ax) / dot(ax, ax), dot(h, ay) / dot(ay, ay));
        // The edges soften with distance too: a satin floor never shows a crisp outline.
        vec2 soft = vec2(0.08 * t) / vec2(length(ax), length(ay)) + 0.01;
        vec2 edge = 1.0 - smoothstep(0.5 - soft, 0.5 + soft, abs(q));
        if (edge.x * edge.y <= 0.0)
            continue;
        best = t;
        q = clamp(q, -0.5, 0.5);
        vec4 sl = wall_slice[i];
        float a = radians(wall_params[i].x), aspect = max(wall_params[i].y, 1e-3);
        vec2 l = vec2(q.x, -q.y) * sl.zw * vec2(aspect, 1.0);
        l = vec2(l.x * cos(a) - l.y * sin(a), l.x * sin(a) + l.y * cos(a)) / vec2(aspect, 1.0);
        float lod = log2(max(t * 0.4 * wall_params[i].w, 1.0)); // blur grows with the distance to the wall
        col = textureLod(composition, sl.xy + 0.5 * sl.zw + l, lod).rgb * wall_params[i].z * edge.x * edge.y;
    }
    float fresnel = 0.04 + 0.96 * pow(1.0 - max(v.y, 0.0), 5.0);
    return col * mix(0.1, 0.8, fresnel); // a satin stage floor: faint looking down, stronger at grazing angles
}
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
    if (light.w > 1.5) { // cinematic: a dark stage floor lit by the rig, reflecting the walls; no CAD grid
        vec3 v = normalize(eye.xyz - v_world);
        vec3 albedo = edge < 0.0 ? vec3(0.35) : vec3(0.2);
        vec3 lit = albedo * 0.03 + rig_light(v_world, vec3(0.0, 1.0, 0.0), v, albedo, 0.3);
        lit += wall_reflection(v_world, v) + vec3(0.03) * outline * fade;
        frag = vec4(lit, 1.0);
        return;
    }
    float limit = 1.0 + 0.6 * light.w; // HDR: pools may pass white a little and bloom
    frag = vec4(c + limit * (1.0 - exp(-1.5 * pools / limit)), 1.0); // soft limit: overlapping pools stop short
}
