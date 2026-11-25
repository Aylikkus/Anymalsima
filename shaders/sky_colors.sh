// xyz is the sun direction, w is the strength of sky light on surfaces.
uniform vec4 u_sky_sun;

vec3 skySunDirection()
{
    // Standalone terrain draws can leave world lighting unbound.
    if (u_sky_sun.w <= 0.0) return normalize(vec3(-0.35, 0.85, 0.4));
    return u_sky_sun.xyz;
}

float skyDaylight()
{
    return u_sky_sun.w > 0.0 ? u_sky_sun.w : 1.0;
}

float skyDirectSun()
{
    return smoothstep(-0.03, 0.16, skySunDirection().y);
}

float skyDayBlend(float sunHeight)
{
    return smoothstep(-0.15, 0.30, sunHeight);
}

vec3 skyHorizon(float sunHeight)
{
    float day = skyDayBlend(sunHeight);
    float twilight = smoothstep(-0.24, -0.04, sunHeight)
                   * (1.0 - smoothstep(0.0, 0.32, sunHeight));
    vec3 horizon = mix(vec3(0.035, 0.052, 0.100),
                       vec3(0.68, 0.77, 0.84), day);
    return mix(horizon, vec3(0.76, 0.37, 0.24), twilight * 0.68);
}
