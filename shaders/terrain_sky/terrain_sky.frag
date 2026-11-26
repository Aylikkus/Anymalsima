$input v_ray

#include <bgfx_shader.sh>
#include "../sky_colors.sh"
#include "../water_fog.sh"
#include "../weather_map.sh"

uniform vec4 u_sky_camera;
uniform vec4 u_sky_seed;
uniform vec4 u_sky_weather; // cloud cover, visual precipitation, snow, hours
uniform vec4 u_sky_cloud_offset; // accumulated ground-parallel drift
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

float cloudHash3(vec3 cell)
{
    vec3 p = fract(cell * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

float cloudNoise3(vec3 position)
{
    vec3 f = fract(position);
    f = f * f * (3.0 - 2.0 * f);
    vec3 cell = floor(position);
    float c000 = cloudHash3(cell);
    float c100 = cloudHash3(cell + vec3(1.0, 0.0, 0.0));
    float c010 = cloudHash3(cell + vec3(0.0, 1.0, 0.0));
    float c110 = cloudHash3(cell + vec3(1.0, 1.0, 0.0));
    float c001 = cloudHash3(cell + vec3(0.0, 0.0, 1.0));
    float c101 = cloudHash3(cell + vec3(1.0, 0.0, 1.0));
    float c011 = cloudHash3(cell + vec3(0.0, 1.0, 1.0));
    float c111 = cloudHash3(cell + vec3(1.0, 1.0, 1.0));
    float bottom = mix(mix(c000, c100, f.x), mix(c010, c110, f.x), f.y);
    float top = mix(mix(c001, c101, f.x), mix(c011, c111, f.x), f.y);
    return mix(bottom, top, f.z);
}

float cloudFbm3(vec3 position)
{
    float value = 0.0;
    float amplitude = 0.55;
    for (int i = 0; i < 3; ++i) {
        value += cloudNoise3(position) * amplitude;
        position = position * 2.03 + vec3(3.7, 1.9, 5.1);
        amplitude *= 0.5;
    }
    return value;
}

vec3 shadeCloud(vec3 sky, vec3 ray, float day, vec3 p,
                float humidity, float precip, float opacity)
{
    float cover = clamp(max(humidity, precip * 0.94), 0.0, 1.0);
    if (cover <= 0.02) return sky;
    p += (u_sky_seed.xyz - vec3_splat(0.5)) * 13.0;
    float shape = cloudFbm3(p * 0.30) * 0.73
                + cloudNoise3(p * 0.92 + vec3(7.3, 2.7, 4.6)) * 0.23;
    // Near the horizon small underside details project into narrow rings.
    // Keep the broad outline and let atmospheric scattering soften detail.
    float detail = smoothstep(0.035, 0.32, abs(ray.y));
    shape = mix(0.47, shape, 0.30 + 0.70 * detail);
    float deck = smoothstep(0.18, 0.83, precip);
    float threshold = mix(0.72, 0.34, cover) - 0.10 * precip;
    float mask = smoothstep(threshold - 0.04, threshold + 0.11, shape);
    // Wet cores have a continuous underside, with noisy contours around
    // their dryer outskirts.
    mask = max(mask, deck * (0.56 + 0.42 * shape)) * opacity;
    mask *= smoothstep(0.13, 0.79, cover);
    if (mask <= 0.001) return sky;

    // Three flat tones: shaded body, lit body and a sun-facing crown/rim.
    float body = smoothstep(threshold + 0.03, threshold + 0.22, shape)
                 * (1.0 - 0.36 * deck);
    float crown = smoothstep(threshold + 0.15, threshold + 0.42, shape)
                  * (1.0 - 0.68 * deck);
    vec3 sunDir = skySunDirection();
    float facing = clamp(dot(ray, sunDir) * 0.5 + 0.5, 0.0, 1.0);

    float storm = clamp(precip * 1.6 + humidity * 0.24, 0.0, 1.0);
    vec3 shade = mix(vec3(0.055, 0.085, 0.20),
                     vec3(0.47, 0.59, 0.78), day);
    vec3 middle = mix(vec3(0.09, 0.14, 0.30),
                      vec3(0.80, 0.86, 0.95), day);
    vec3 lit = mix(vec3(0.17, 0.25, 0.45),
                   vec3(1.0, 0.97, 0.89), day);
    shade = mix(shade, mix(vec3(0.06, 0.08, 0.13),
                           vec3(0.32, 0.37, 0.45), day), storm * 0.85);
    middle = mix(middle, mix(vec3(0.09, 0.12, 0.19),
                             vec3(0.47, 0.52, 0.59), day), storm * 0.80);
    lit = mix(lit, mix(vec3(0.15, 0.19, 0.28),
                       vec3(0.68, 0.72, 0.77), day), storm * 0.70);

    vec3 colour = mix(shade, middle, body);
    colour = mix(colour, lit, crown * (0.50 + 0.50 * facing));
    colour *= 1.0 + 0.06 * max(dot(ray, sunDir), 0.0) * day;
    return mix(sky, colour, mask);
}

vec3 skyClouds(vec3 sky, vec3 ray, float day)
{
    if (u_weather_grid.z <= 0.0) {
        if (ray.y <= 0.0) return sky;
        vec2 drift = u_sky_cloud_offset.xy;
        vec3 p = ray * 9.0 + vec3(drift.x, 0.0, drift.y);
        return shadeCloud(sky, ray, day, p, u_sky_weather.x,
                          u_sky_weather.y, smoothstep(0.0, 0.08, ray.y));
    }

    if (abs(ray.y) < 0.001) return sky;
    float horizon = smoothstep(0.012, 0.12, abs(ray.y));
    // Sample the world cloud deck at a few elevations. A texel's cloud base
    // limits the layers it can fill, keeping the clouds over their rain.
    for (int layer = 0; layer < 6; ++layer) {
        float order = ray.y > 0.0 ? 5.0 - float(layer) : float(layer);
        float y = WEATHER_MIN_CLOUD_BASE + 33.0 + order * 102.0;
        float t = (y - u_weather_grid.w) / ray.y;
        if (t <= 0.0) continue;
        vec2 pXZ = ray.xz * t;
        vec4 weather = weatherAt(pXZ, false);
        float precipitation = clamp(weather.y + weather.z, 0.0, 1.0);
        if (weather.x + precipitation <= 0.04) continue;
        float base = weatherCloudBase(weather.a) + u_weather_grid.w;
        float vertical = 1.0 - smoothstep(28.0, 105.0, abs(y - base - 49.0));
        if (vertical <= 0.0) continue;

        // Cylindrical X keeps the cloud noise continuous across the X seam.
        vec2 world = u_weather_anchor.xy + pXZ - u_sky_cloud_offset.xy * 6.0;
        float scale = 0.033;
        float angle = world.x * 6.2831853 / u_weather_anchor.z;
        float radius = u_weather_anchor.z * scale / 6.2831853;
        vec3 p = vec3(sin(angle) * radius, cos(angle) * radius,
                      world.y * scale + y * 0.016);
        sky = shadeCloud(sky, ray, day, p, weather.x, precipitation,
                         horizon * vertical * 0.88);
    }
    return sky;
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
    sky = skyClouds(sky, ray, day);
    gl_FragColor = vec4(clamp(sky, 0.0, 1.0), 1.0);
}
