// A weather texel carries cloud cover, rain density, snow density and base altitude.
uniform vec4 u_weather_grid;   // eye UV, inverse width in metres, eye altitude
uniform vec4 u_weather_limits; // relative Z bounds, horizontal range, fixture flag
uniform vec4 u_weather_anchor; // canonical eye X, world eye Z, X wrap period
SAMPLER2D(u_weather_map, 2);

#define WEATHER_MIN_CLOUD_BASE 540.0
#define WEATHER_MAX_CLOUD_BASE 1020.0

float weatherCloudBase(float height)
{
    return mix(WEATHER_MIN_CLOUD_BASE, WEATHER_MAX_CLOUD_BASE, height)
           - u_weather_grid.w;
}

vec4 weatherAt(vec2 p, bool sharpFixture)
{
    if (p.y < u_weather_limits.x || p.y >= u_weather_limits.y
        || dot(p, p) >= u_weather_limits.z * u_weather_limits.z)
        return vec4_splat(0.0);
    vec2 uv = u_weather_grid.xy + p * u_weather_grid.z;
    float distance = length(p);
    float fade = 1.0 - smoothstep(u_weather_limits.z * 0.72,
                                  u_weather_limits.z, distance);
    vec4 weather = texture2D(u_weather_map, uv);
    if (sharpFixture && u_weather_limits.w > 0.5) {
        // The four-region test has sharply defined, feathered boundaries.
        float cells = 1.0 / (u_weather_grid.z * 128.0);
        vec2 tile = uv * cells;
        vec2 edge = min(fract(tile), 1.0 - fract(tile));
        vec2 centre = (floor(tile) + 0.5) / cells;
        weather.yz = texture2D(u_weather_map, centre).yz
                     * smoothstep(0.0, 8.0 / 128.0, min(edge.x, edge.y));
    }
    return vec4(weather.rgb * fade, weather.a);
}
