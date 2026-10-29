#version 440
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_colour;
layout(location = 2) in vec3 v_world;
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
#include "lights.glsl"
void main() {
    vec3 n = normalize(v_normal);
    float diffuse = max(dot(n, normalize(light.xyz)), 0.0);
    float fill = max(dot(n, normalize(eye.xyz - v_world)), 0.0);
    vec3 lit = v_colour.rgb * (0.28 + 0.55 * diffuse + 0.25 * fill);
    float emissive = clamp(v_colour.a - 1.0, 0.0, 1.0); // alpha 2: emissive
    if (light.w > 1.5) { // cinematic: a dark room lit by the rig, emissive surfaces glowing
        vec3 v = normalize(eye.xyz - v_world);
        if (dot(n, v) < 0.0)
            n = -n; // set pieces are double sided
        vec3 c = v_colour.rgb * (0.035 + 0.05 * diffuse) + rig_light(v_world, n, v, v_colour.rgb, 0.55);
        frag = vec4(mix(c, v_colour.rgb * 4.0, emissive), 1.0);
        return;
    }
    if (light.w > 0.5) { // HDR: a little sheen, and emissive surfaces bright enough to bloom
        vec3 h = normalize(normalize(light.xyz) + normalize(eye.xyz - v_world));
        lit += vec3(0.18) * pow(max(dot(n, h), 0.0), 40.0);
        frag = vec4(mix(lit, v_colour.rgb * 1.8, emissive), 1.0);
        return;
    }
    frag = vec4(mix(lit, v_colour.rgb, emissive), 1.0);
}
