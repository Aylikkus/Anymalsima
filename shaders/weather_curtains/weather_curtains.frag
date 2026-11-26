$input v_position, v_depth

#include <bgfx_shader.sh>
#include "../weather_map.sh"

uniform vec4 u_curtain_fade; // near fade start/end, cloud thickness
uniform vec4 u_curtain_time;
uniform vec4 u_sky_sun;

float rainHash(vec2 p, float columns)
{
    p.x = mod(p.x, columns);
    vec3 h = fract(p.xyx * 0.1031);
    h += dot(h, h.yzx + 33.33);
    return fract((h.x + h.y) * h.z);
}

float rainBands(vec2 p, float columns)
{
    vec2 tile = floor(p);
    vec2 f = smoothstep(vec2_splat(0.0), vec2_splat(1.0), fract(p));
    return mix(mix(rainHash(tile, columns),
                   rainHash(tile + vec2(1.0, 0.0), columns), f.x),
               mix(rainHash(tile + vec2(0.0, 1.0), columns),
                   rainHash(tile + vec2(1.0, 1.0), columns), f.x), f.y);
}

void main()
{
    float distance = length(v_position.xz);
    if (distance <= u_curtain_fade.x || distance >= u_weather_limits.z
        || v_position.y + u_weather_grid.w >= WEATHER_MAX_CLOUD_BASE
                                                + u_curtain_fade.z) discard;
    vec4 weather = weatherAt(v_position.xz, true);
    if (weather.y + weather.z < 0.002) discard;
    float height = v_position.y - weatherCloudBase(weather.a);

    float fade = smoothstep(u_curtain_fade.x, u_curtain_fade.y, distance)
                 * (1.0 - smoothstep(-35.0, u_curtain_fade.z, height));
    // Continuous extinction gives the far field a misty body. Irregular,
    // falling flecks add detail at the near end where they can be resolved.
    float time = u_curtain_time.x;
    vec2 world = u_weather_anchor.xy + v_position.xz;
    float wrappedX = world.x / u_weather_anchor.z;
    float streakColumns = max(2.0, floor(u_weather_anchor.z * 0.53 + 0.5));
    vec2 streakUv = vec2(wrappedX * streakColumns + world.y * 0.41 + height * 0.015,
                         (v_position.y + u_weather_grid.w) * 0.18 + time * 2.0);
    vec2 cell = floor(streakUv);
    vec2 q = fract(streakUv);
    float seed = rainHash(cell, streakColumns);
    float centre = 0.28 + 0.44 * rainHash(cell + vec2(17.0, 29.0), streakColumns);
    float fall = abs(q.y - centre);
    float streak = (1.0 - smoothstep(0.03, 0.095, abs(q.x - mix(0.14, 0.86, seed))))
                   * (1.0 - smoothstep(0.03, 0.23, fall));
    float detailFade = 1.0 - smoothstep(47.0, 155.0, distance);
    float bandColumns = max(2.0, floor(u_weather_anchor.z * 0.017 + 0.5));
    float shafts = rainBands(vec2(wrappedX * bandColumns, world.y * 0.017),
                            bandColumns);
    float rainAmount = weather.y * (0.00135 + 0.0012 * shafts
                                   + 0.0015 * streak * detailFade);
    float snowAmount = weather.z * 0.00118;
    float day = clamp(u_sky_sun.w, 0.0, 1.0);
    vec3 rain = mix(vec3(0.18, 0.24, 0.33), vec3(0.50, 0.58, 0.66), day);
    vec3 snow = mix(vec3(0.43, 0.55, 0.74), vec3(0.88, 0.93, 0.98), day);
    vec3 colour = mix(rain, snow, snowAmount / (rainAmount + snowAmount));
    float thickness = min(1.6, length(v_position) / v_depth.x) * v_depth.y;
    float alpha = 1.0 - exp(-fade * thickness * (rainAmount + snowAmount));
    gl_FragColor = vec4(colour, alpha);
}
