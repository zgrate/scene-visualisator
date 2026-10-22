#version 440
layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in vec4 v_slice;
layout(location = 2) flat in vec4 v_grid;
layout(location = 3) flat in vec4 v_params; // brightness, selected, slice angle (degrees), composition aspect
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
layout(binding = 1) uniform sampler2D composition;
void main() {
    if (!gl_FrontFacing) {
        frag = vec4(0.08, 0.09, 0.11, 1.0);
        return;
    }
    vec2 px = v_uv * v_grid.xy;
    // Pixel centre relative to the slice centre, rotated by the slice angle in square-pixel units.
    float a = radians(v_params.z), aspect = max(v_params.w, 1e-3);
    vec2 l = ((floor(px) + 0.5) / v_grid.xy - 0.5) * v_slice.zw * vec2(aspect, 1.0);
    l = vec2(l.x * cos(a) - l.y * sin(a), l.x * sin(a) + l.y * cos(a)) / vec2(aspect, 1.0);
    vec3 c = texture(composition, v_slice.xy + 0.5 * v_slice.zw + l).rgb * v_params.x;
    // LED dot mask, faded out once a pixel is smaller than ~3 screen pixels.
    float cell = max(fwidth(px.x), fwidth(px.y));
    float dots = 1.0 - smoothstep(0.3, 0.62, length(fract(px) - 0.5));
    c *= mix(1.0, 0.25 + 0.75 * dots, clamp(1.0 - cell * 3.0, 0.0, 1.0));
    vec2 m = v_uv * v_grid.zw;
    vec2 seam = abs(fract(m - 0.5) - 0.5) / fwidth(m);
    c *= mix(0.55, 1.0, min(min(seam.x, seam.y), 1.0));
    if (v_params.y > 0.5) {
        vec2 e = min(v_uv, 1.0 - v_uv) / fwidth(v_uv);
        if (min(e.x, e.y) < 3.0)
            c = vec3(0.298, 0.553, 1.0); // Theme accent
    }
    frag = vec4(c, 1.0);
}
