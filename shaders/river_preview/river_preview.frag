$input v_color0, v_texcoord0  // v_color0 contains packed vec4 (_, flow, side, height)

#include <bgfx_shader.sh>

uniform vec4 u_time;          // Time packed as vec4
uniform vec4 u_zoom;          // Zoom packed as vec4

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(hash(i),              hash(i + vec2(1.0, 0.0)), u.x),
        mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x),
        u.y);
}

// ── mirrors waterColor() from the base shader so the blend target matches ──
vec3 seaColor(vec2 wp, float h)
{
    float z   = clamp(u_zoom.x, 0.1, 50.0);
    float lod = smoothstep(2.0, 40.0, z);
    vec2  p   = wp * 0.5;
    float t   = u_time.x * 0.12;

    float wLow  = noise(p * 0.6 + t);
    float wMid  = noise(p * 1.4 + vec2(t, -t));
    float w     = mix(wLow, wMid, lod);
    w = w * w * (3.0 - 2.0 * w);

    float deep = smoothstep(0.0, 0.33, h);
    vec3 col = mix(vec3(0.04, 0.08, 0.20), vec3(0.07, 0.22, 0.48), deep);
    col += vec3(0.01, 0.015, 0.02) * w * mix(0.008, 0.03, lod);
    return col;
}

void main()
{
    float river = step(0.5, v_color0.r);
    if (river < 0.49) discard;

    float flow   = v_color0.g;
    float side   = v_color0.b * 2.0 - 1.0; // –1(left)..+1(right)
    float h      = v_color0.a;
    vec2  p      = v_texcoord0;

    float zoomFade = smoothstep(5.0, 15.0, u_zoom.x);
    // ── fade window just above the water threshold ──
    // fully visible at h >= 0.55, fully gone at h <= 0.49
    float riverAlpha = smoothstep(0.49, 0.55, h);

    // nothing to draw — let the base water show through
    if (riverAlpha < 0.001) discard;

    float lod = smoothstep(3.0, 25.0, u_zoom.x);
    float t   = u_time.x * 0.15;

    float n = noise(p * 0.6 + vec2(t * (0.5 + flow), t       )) * 0.6
            + noise(p * 1.2 - vec2(t * (0.5 + flow), t * 0.7 )) * 0.4;
    float wave = n * n * (3.0 - 2.0 * n);

    float center     = 1.0 - abs(side);
    float widthShape = smoothstep(0.0, 1.0, center);

    vec3 col = mix(vec3(0.04, 0.08, 0.20), vec3(0.07, 0.22, 0.48),
                   widthShape * 0.6);
    col += vec3(0.02,  0.03,  0.05 ) * widthShape * flow;
    col += vec3(0.01,  0.015, 0.025) * wave;
    col  = mix(vec3(0.06, 0.15, 0.32), col, lod);

    // ── blend river → sea color as it approaches h = 0.5 ──
    // this hides the seam where river geometry overlaps coastal water
    vec3 sea      = seaColor(p, h);
    float seaBlend  = smoothstep(0.55, 0.49, h);
    col = mix(col, sea, seaBlend);

    float alpha = zoomFade;
    if (h < 0.49) discard;
    gl_FragColor = vec4(col, alpha);
}