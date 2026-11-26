$input v_uv, v_data

#include <bgfx_shader.sh>

uniform vec4 u_sky_sun;      // w: strength of outdoor sky light
uniform vec4 u_sky_weather;  // x: cloud cover

void main()
{
    float kind = v_data.y;
    float day = clamp(u_sky_sun.w, 0.0, 1.0);
    float cloud = clamp(u_sky_weather.x, 0.0, 1.0);
    vec3 colour;
    float alpha;

    if (kind > 0.5) {
        // A soft round flake, cool at night and warm-white in daylight.
        float flake = 1.0 - smoothstep(0.42, 1.0, length(v_uv));
        alpha = v_data.x * flake;
        colour = mix(vec3(0.58, 0.70, 0.92), vec3(1.0, 0.98, 0.93), day);
        colour *= mix(1.0, 0.84, cloud * 0.6);
    } else {
        // A thin streak with a brighter leading tip.
        float streak = 1.0 - smoothstep(0.25, 0.95, abs(v_uv.x));
        float tail = 0.55 + 0.45 * (0.5 - v_uv.y * 0.5);
        alpha = v_data.x * streak * tail;
        colour = mix(vec3(0.34, 0.45, 0.68), vec3(0.70, 0.79, 0.91), day);
        colour *= mix(1.0, 0.78, cloud * 0.6);
    }

    if (alpha <= 0.002) discard;
    gl_FragColor = vec4(colour, min(alpha, 0.9));
}
