$input v_ray

#include <bgfx_shader.sh>
#include "../sky_colors.sh"
#include "../water_fog.sh"

uniform vec4 u_sky_camera;
uniform vec4 u_sky_seed;
SAMPLER2D(u_sky_sun_map, 0);
SAMPLER2D(u_sky_moon_map, 1);

vec3 starHash(vec2 cell)
{
    vec3 p = fract(vec3(cell.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

vec3 starField(vec2 uv, float columns, float angularPixel, vec2 seed,
               float density)
{
    vec2 grid = uv * vec2(columns, columns * 0.5);
    vec2 cell = floor(grid);
    cell.x = mod(cell.x, columns);
    vec3 star = starHash(cell + seed);
    if (star.z < 1.0 - density) return vec3_splat(0.0);

    float variation = (star.z - (1.0 - density)) / density;
    float radius = mix(0.035, 0.18, pow(variation, 5.0));
    float pixel = clamp(angularPixel * (columns / 6.2831853), 0.01, 0.13);
    vec2 offset = fract(grid) - mix(vec2_splat(0.32), vec2_splat(0.68),
                                    star.xy);
    float distance = length(offset);
    float core = 1.0 - smoothstep(max(radius - pixel, 0.0),
                                 radius + pixel, distance);
    float brightness = mix(0.16, 0.94, variation * variation);
    // Blue, lavender, rose and gold stars. A soft outer tint separates the
    // brightest cores from the surrounding star field.
    vec3 cool = mix(vec3(0.49, 0.73, 1.0), vec3(0.82, 0.65, 1.0),
                    smoothstep(0.0, 0.54, star.y));
    vec3 tint = mix(cool, vec3(1.0, 0.79, 0.52),
                    smoothstep(0.54, 1.0, star.y));
    float glow = smoothstep(0.84, 1.0, variation)
               * (1.0 - smoothstep(radius, 0.31, distance)) * 0.34;
    return mix(tint, vec3_splat(1.0), 0.5 * variation) * core * brightness
           + tint * glow;
}

vec3 skyStars(vec3 ray, float angularPixel)
{
    const float pi = 3.14159265;
    // Put the panorama poles on the horizon where the stars fade out.
    vec2 uv = vec2(0.5 + atan2(ray.x, ray.y) / (2.0 * pi),
                   0.5 - asin(clamp(ray.z, -1.0, 1.0)) / pi);
    vec2 seed = u_sky_seed.zw * 317.0;
    return starField(uv, 384.0, angularPixel, seed, 0.14)
         + starField(uv, 768.0, angularPixel, seed.yx + 71.0, 0.22) * 0.55;
}

vec4 galaxyHash(float cell)
{
    vec4 p = fract((cell + vec4(0.0, 17.0, 59.0, 76.0)) * 0.1031);
    p *= p + 33.33;
    return fract(p * (p + p));
}

float galaxyNoise(vec3 position)
{
    vec3 f = fract(position);
    f = f * f * (3.0 - 2.0 * f);
    float cell = dot(floor(position), vec3(1.0, 17.0, 59.0));
    vec4 corners = mix(galaxyHash(cell), galaxyHash(cell + 1.0), f.x);
    vec2 alongY = mix(corners.xz, corners.yw, f.y);
    return mix(alongY.x, alongY.y, f.z);
}

vec3 galaxyLight(vec3 ray)
{
    float azimuth = u_sky_seed.x * 6.2831853;
    float altitude = mix(0.46, 0.92, u_sky_seed.y);
    float tilt = mix(0.62, 1.23, u_sky_seed.z);
    vec3 horizontal = vec3(cos(azimuth), 0.0, sin(azimuth));
    vec3 core = vec3(horizontal.x * cos(altitude), sin(altitude),
                     horizontal.z * cos(altitude));
    vec3 east = vec3(-horizontal.z, 0.0, horizontal.x);
    vec3 alongBand = east * cos(tilt) + cross(east, core) * sin(tilt);
    vec3 acrossBand = cross(core, alongBand);
    vec3 direction = vec3(dot(ray, core), dot(ray, alongBand),
                          dot(ray, acrossBand));
    float bulge = pow(max(direction.x, 0.0), 5.0);
    float width = 0.065 + bulge * 0.094;
    if (abs(direction.z) > width * 4.0 + 0.07)
        return vec3_splat(0.0);

    vec3 p = direction * vec3(8.0, 8.0, 14.0)
           + u_sky_seed.yzw * 29.0;
    float broad = galaxyNoise(p);
    float structure = galaxyNoise(p * 2.15 + vec3(5.5, 2.8, 7.3));
    float detail = galaxyNoise(p * 5.1 + vec3(2.6, 4.9, 8.1));
    float grain = galaxyNoise(p * 12.0 + vec3(0.8, 5.2, 3.8));
    float latitude = direction.z + (broad - 0.5) * 0.073;
    float band = exp(-abs(latitude) / width);
    float knots = smoothstep(0.27, 0.70,
        0.46 * broad + 0.33 * structure + 0.15 * detail + 0.06 * grain);
    // Dust cuts a broken lane through the illuminated galactic plane.
    float laneOffset = (structure - 0.5) * 0.078;
    float dust = exp(-abs(latitude - laneOffset) / (width * 0.31))
               * smoothstep(0.24, 0.69, detail * 0.75 + grain * 0.25);
    float light = band * (0.033 + knots * (0.22 + bulge * 0.74))
                * (1.0 - dust * 0.92);
    light *= 1.0 - smoothstep(width * 3.1 + 0.07, width * 4.0 + 0.07,
                              abs(direction.z));
    vec3 tint = mix(vec3(0.39, 0.60, 1.0), vec3(0.86, 0.55, 1.0),
                    smoothstep(0.27, 0.64, structure));
    tint = mix(tint, vec3(1.0, 0.81, 0.66), bulge * knots);
    return tint * light;
}

vec3 skySprite(vec3 sky, vec3 ray, vec3 direction, bool moon)
{
    const float halfExtent = 0.09;
    float facing = dot(ray, direction);
    if (facing < 0.99) return sky;

    vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), direction));
    vec3 up = cross(direction, right);
    vec2 plane = vec2(dot(ray, right), dot(ray, up)) / (facing * halfExtent);
    if (max(abs(plane.x), abs(plane.y)) > 1.0) return sky;
    vec2 uv = plane * vec2(0.5, -0.5) + 0.5;
    vec4 art;
    if (moon) art = texture2D(u_sky_moon_map, uv);
    else art = texture2D(u_sky_sun_map, uv);
    float aboveHorizon = smoothstep(-0.007, 0.012, ray.y);
    return mix(sky, art.rgb, art.a * aboveHorizon);
}

void main()
{
    if (u_sky_camera.w > 0.5) {
        gl_FragColor = vec4(WATER_FOG_COLOR, 1.0);
        return;
    }
    vec3 ray = normalize(v_ray);
    float angularPixel = max(length(dFdx(ray)), length(dFdy(ray)));
    vec3 sun = skySunDirection();
    float day = skyDayBlend(sun.y);
    vec3 zenith = mix(vec3(0.010, 0.018, 0.057),
                      vec3(0.19, 0.43, 0.77), day);
    vec3 sky = mix(skyHorizon(sun.y), zenith, smoothstep(0.0, 0.85, ray.y));
    float night = 1.0 - smoothstep(-0.30, -0.05, sun.y);
    if (night > 0.0 && ray.y > 0.0) {
        vec3 galaxy = galaxyLight(ray);
        vec3 stars = skyStars(ray, angularPixel);
        sky += (galaxy + stars * (1.0 + galaxy.b * 0.5)) * night
             * smoothstep(0.0, 0.18, ray.y);
    }
    if (sun.y > -0.13) sky = skySprite(sky, ray, sun, false);
    if (sun.y < 0.13) sky = skySprite(sky, ray, -sun, true);
    gl_FragColor = vec4(clamp(sky, 0.0, 1.0), 1.0);
}
