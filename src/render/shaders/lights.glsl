// The rig as lights, for the cinematic branch (light.w > 1.5) of mesh, grid and wall. Matches LightUniforms in
// scene_renderer.cpp. Filled only in cinematic; the other branches never read it.
layout(std140, binding = 2) uniform Lights {
    vec4 counts;       // beams, wall cells, wall quads
    vec4 beam_pos[48]; // origin, length (where the beam stops)
    vec4 beam_dir[48]; // unit direction, tan of the half angle
    vec4 beam_col[48]; // colour, already scaled by intensity
    vec4 cell_pos[64]; // centre, area in m²
    vec4 cell_dir[64]; // facing normal
    vec4 cell_col[64]; // mean composition colour × brightness
    vec4 wall_c[16];   // wall quads, largest first: centre, local x and y edge vectors (front faces x × y)
    vec4 wall_x[16];
    vec4 wall_y[16];
    vec4 wall_slice[16];  // composition rect in 0..1
    vec4 wall_params[16]; // slice angle (degrees), composition aspect, brightness, composition texels per metre
};

const float pi = 3.14159265;

// GGX specular with Schlick Fresnel (F0 0.04, a dielectric), Smith-Schlick visibility.
float ggx(vec3 n, vec3 v, vec3 l, float rough) {
    vec3 h = normalize(v + l);
    float nh = max(dot(n, h), 0.0), nv = max(dot(n, v), 1e-3), nl = max(dot(n, l), 1e-3);
    float a2 = rough * rough * rough * rough;
    float dd = nh * nh * (a2 - 1.0) + 1.0;
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float vis = 1.0 / ((nv * (1.0 - k) + k) * (nl * (1.0 - k) + k));
    float fresnel = 0.04 + 0.96 * pow(1.0 - max(dot(h, v), 0.0), 5.0);
    return a2 / (pi * dd * dd) * vis * 0.25 * fresnel;
}

// Light reaching point p (normal n, toward the eye v) from every beam and LED wall cell, reflected by a surface of
// the given albedo and roughness. A beam only lights points up to where it stops: no shadow maps, but a beam that lands
// on a deck leaves the floor under it dark.
vec3 rig_light(vec3 p, vec3 n, vec3 v, vec3 albedo, float rough) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < int(counts.x); ++i) {
        vec3 to = p - beam_pos[i].xyz, d = beam_dir[i].xyz;
        float s = dot(to, d);
        if (s <= 0.0 || s > beam_pos[i].w + 0.15)
            continue;
        float tan_half = max(beam_dir[i].w, 0.02);
        float q = length(to - d * s) / (0.06 + s * tan_half);
        if (q >= 1.0)
            continue;
        float d2 = dot(to, to);
        vec3 l = -to * inversesqrt(d2);
        float nl = dot(n, l);
        if (nl <= 0.0)
            continue;
        // The same flux in a narrower cone is brighter; inverse square, softened near the lens.
        vec3 e = beam_col[i].rgb * (1.0 - smoothstep(0.75, 1.0, q)) * 4.0 / (tan_half * tan_half) / (d2 + 1.0);
        sum += e * nl * (albedo + ggx(n, v, l, rough));
    }
    for (int i = 0; i < int(counts.y); ++i) {
        vec3 to = cell_pos[i].xyz - p;
        float d2 = max(dot(to, to), 1e-4);
        vec3 l = to * inversesqrt(d2);
        float nl = dot(n, l);
        float facing = dot(cell_dir[i].xyz, -l);
        if (nl <= 0.0 || facing <= 0.0)
            continue;
        float area = cell_pos[i].w;
        // A small area light: cosine at both ends, and A / (pi d² + A) keeps it finite up close.
        vec3 e = cell_col[i].rgb * 6.0 * area * facing / (pi * d2 + area);
        sum += e * nl * (albedo + ggx(n, v, l, max(rough, 0.3)));
    }
    return sum;
}
