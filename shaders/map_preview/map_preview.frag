$input v_color0, v_texcoord0  // v_color0 contains packed vec4 (biome, moisture, temperature, height)

#include <bgfx_shader.sh>

uniform vec4 u_time;          // Time packed as vec4
uniform vec4 u_zoom;          // Zoom packed as vec4
uniform vec4 u_world_width;   // World width packed as vec4
uniform vec4 u_min_temp_c;    // Minimum temperature packed as vec4
uniform vec4 u_max_temp_c;    // Maximum temperature packed as vec4

float hash(vec2 p)
{
    p.x = mod(p.x, u_world_width.x);

    return fract(
        sin(dot(p, vec2(127.1, 311.7))) * 43758.5453
    );
}
float noise(vec2 p)
{
    p.x = mod(p.x, u_world_width.x);
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i),        hash(i + vec2(1,0)), u.x),
               mix(hash(i + vec2(0,1)), hash(i + vec2(1,1)), u.x), u.y);
}
float fbm(vec2 p)
{
    p.x = mod(p.x, u_world_width.x);

    float v = 0.0, a = 0.5;
    float scale_factor = 1.0;
    for (int i = 0; i < 5; i++)
    {
        v += a * noise(p);
        p *= 2.1;
        scale_factor *= 2.1;
        p.x = mod(p.x, u_world_width.x * scale_factor);  // wrap at scaled period
        a *= 0.5;
    }
    return v;
}

// In your base shader, add a variant that takes an explicit wrap width:
float hash_w(vec2 p, float period) {
    p.x = mod(p.x, period);
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float noise_w(vec2 p, float period) {
    p.x = mod(p.x, period);
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash_w(i,           period), hash_w(i+vec2(1,0), period), u.x),
               mix(hash_w(i+vec2(0,1), period), hash_w(i+vec2(1,1), period), u.x), u.y);
}
float fbm_w(vec2 p, float period) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * noise_w(p, period);
        p       *= 2.1;
        period  *= 2.1;   // scale period with p so wrap stays correct
        a       *= 0.5;
    }
    return v;
}


vec3 waterColor(vec2 wp, float h, float temp01, float biome, float tempC)
{
    float z = clamp(u_zoom.x, 0.1, 50.0);

    // ─────────────────────────────
    // ICE SHEET (solid, snow-like, slightly gray)
    // ─────────────────────────────
    if (tempC < -15.0) // ICE_SHEET = 250
    {
        float n = fbm(wp * 4.0);

        vec3 ice = vec3(0.78, 0.82, 0.85); // grayish snow
        vec3 snow = vec3(0.88, 0.92, 0.95);

        // subtle variation so it's not flat
        float mixv = smoothstep(0.3, 0.7, n);

        vec3 col = mix(ice, snow, mixv * 0.4);

        // very slight shading from height (keeps terrain readable)
        col *= 0.95 + h * 0.1;

        return col;
    }

    float basePeriod = u_world_width.x;
    // ─────────────────────────────
    // SEA ICE (patchy, floating over water)
    // ─────────────────────────────
    if (tempC < -5.0) // SEA_ICE = 220
    {
        float lod = smoothstep(2.0, 40.0, z);

        vec2 p = wp * 0.6;
        float t = u_time.x * 0.05;

        // base water (reduced detail)
        float w = noise(p + t);
        vec3 water = mix(vec3(0.04, 0.08, 0.20),
                         vec3(0.07, 0.22, 0.48),
                         smoothstep(0.0, 0.33, h));

        
        // ice mask (patches)
        float n = fbm_w(p * 2.5 + vec2(t, -t), basePeriod * 2.5 * 0.6);

        // sharper threshold → distinct chunks
        float iceMask = step(0.55, n);

        // soften edges slightly (avoid aliasing only)
        iceMask = smoothstep(0.55, 0.65, n);

        vec3 ice = vec3(0.82, 0.86, 0.90);

        // mix ice over water
        vec3 col = mix(water, ice, iceMask * 0.9);

        // slight brightness variation per chunk
        col += (n - 0.5) * 0.05 * iceMask;

        return col;
    }

    // ── LOD factor: 0 (far) → 1 (close zoom)
    float lod = smoothstep(2.0, 40.0, z);

    vec2 p = wp * 0.5;

    float t = u_time.x * 0.12;

    // ── domain warp (reduced influence, prevents shimmer)
    vec2 warp;
    warp.x = noise_w(p * 0.8 + t, basePeriod * 0.8 * 0.5);
    warp.y = noise_w(p * 0.8 - t, basePeriod * 0.8 * 0.5);
    p += (warp - 0.5) * (0.4 + 0.3 * lod);

    vec2 t2 = vec2(t, t);

    float wLow  = noise_w(p * 0.6 + t2, basePeriod * 0.6 * 0.5);
    float wMid  = noise_w(p * 1.4 + t2, basePeriod * 1.4 * 0.5);
    float wHigh = noise_w(p * 4.0 + t2, basePeriod * 4.0 * 0.5) - 0.5;

    // blend by zoom
    float w = mix(wLow, wMid, lod);
    w += wHigh * lod;

    // shape without destroying detail
    w = w * w * (3.0 - 2.0 * w);

    float motion = w;

    // ── base color
    float deep = smoothstep(0.0, 0.33, h);

    vec3 cDeep    = vec3(0.04, 0.08, 0.20);
    vec3 cShallow = vec3(0.07, 0.22, 0.48);

    vec3 col = mix(cDeep, cShallow, deep);

    // ── far view: suppress color noise completely
    float colorStrength = mix(0.008, 0.03, lod);
    col += vec3(0.01, 0.015, 0.02) * motion * colorStrength;

    // ── coastline stability fix (important)
    float coastN = noise_w(p * 1.5, basePeriod * 1.5 * 0.5);
    float hWrap = h; // height is geometry, not periodic

    float coast = smoothstep(
        0.5,
        0.5 + fwidth(hWrap) * 2.0,
        hWrap + (coastN - 0.5) * 0.03
    );

    vec3 foam = vec3(0.75, 0.85, 0.92);
    col = mix(col, foam, coast * 0.45);

    // ── specular (clamped, no sparkle)
    float spec = pow(max(motion, 0.0), 2.5);
    col += spec * 0.05;

    return col;
}


vec3 landColor(vec2 wp, float h, float temp01, float moist, float biome, float tempC)
{
    float n  = fbm(wp * mix(2.0, 6.0, smoothstep(5.0, 40.0, u_zoom.x))) * 0.05;
    float hv = h + n;

    float isDunes = smoothstep(0.30, 0.42, biome);
    vec2 windDir = normalize(vec2(0.8, 0.6));

    float a = dot(wp * 0.8, windDir);
    float b = dot(wp * 0.8, vec2(-windDir.y, windDir.x));
    float dunes = fbm(vec2(a * 3.0, b * 0.5));
    dunes = smoothstep(0.45, 0.75, dunes);

    float duneSlope = length(vec2(dFdx(dunes), dFdy(dunes))) * 30.0;
    float duneShade = 1.0 - clamp(duneSlope, 0.0, 0.35);

    vec3 cSandLight = vec3(0.86, 0.78, 0.55);
    vec3 cSandDark  = vec3(0.70, 0.60, 0.40);

    vec3 sand = mix(cSandDark, cSandLight, dunes) * duneShade;

    // ─────────────────────────────
    // base terrain (non-vegetation)
    // ─────────────────────────────
    vec3 cSoil = vec3(0.42, 0.38, 0.30);
    vec3 cRock = vec3(0.47, 0.47, 0.47);
    vec3 cPeak = vec3(0.87, 0.92, 0.95);

    float tMount = smoothstep(0.66, 0.75, hv);
    float tPeak  = smoothstep(0.72, 0.86, hv);

    vec3 base = mix(cSoil, cRock, tMount);
    base = mix(base, cPeak, tPeak);

    // ─────────────────────────────
    // vegetation palettes
    // ─────────────────────────────
    vec3 cGrass      = vec3(0.29, 0.54, 0.25);
    vec3 cShrub      = vec3(0.36, 0.42, 0.22);
    vec3 cForest     = vec3(0.18, 0.38, 0.18);
    vec3 cRainforest = vec3(0.10, 0.70, 0.20);

    // ─────────────────────────────
    // EXACT rule replication
    // ─────────────────────────────

    // no vegetation conditions
    float noVeg = step(moist, 0.2) + step(tempC, -10.0);
    noVeg = clamp(noVeg, 0.0, 1.0);

    // moisture bands (softened slightly to avoid hard seams)
    float grassBand = smoothstep(0.2, 0.4, moist) * (1.0 - smoothstep(0.4, 0.45, moist));
    float shrubBand = smoothstep(0.4, 0.6, moist) * (1.0 - smoothstep(0.6, 0.65, moist));
    float forestBand = smoothstep(0.6, 0.8, moist);

    // temperature modifier for forest/rainforest
    float warmEnough = smoothstep(15.0, 25.0, tempC);

    float rainforestBand = forestBand * warmEnough;
    float forestOnlyBand = forestBand * (1.0 - warmEnough);

    // normalize so they don’t overlap too much
    float total =
        grassBand +
        shrubBand +
        forestOnlyBand +
        rainforestBand;

    total = max(total, 0.0001);

    grassBand      /= total;
    shrubBand      /= total;
    forestOnlyBand /= total;
    rainforestBand /= total;

    // combine vegetation color
    vec3 vegColor =
        cGrass      * grassBand +
        cShrub      * shrubBand +
        cForest     * forestOnlyBand +
        cRainforest * rainforestBand;
        
    vec3 land = mix(base, vegColor, 1.0 - noVeg);

    // dunes only in low elevation
    float duneMask = isDunes * (1.0 - tMount);

    land = mix(land, sand, duneMask);

    // slope darkening
    float slope = length(vec2(dFdx(h), dFdy(h))) * 120.0;
    land *= 1.0 - clamp(slope * 0.4, 0.0, 0.35);

    // peaks override
    land = mix(land, cPeak, tPeak);

    return land;
}


vec3 applySnow(vec3 col, vec2 wp, float h, float temp01, float moist, float tempC)
{
    if (h >= 0.5)
    {
        // ── noise field (controls patch breakup)
        float n = fbm(wp * 6.0);

        // ── temperature influence (colder → stronger snow)
        float cold = smoothstep(2.0, -15.0, tempC);
        
        float moistureGate = step(0.1, moist);

        // optional smooth ramp AFTER cutoff (prevents harsh edge)
        float moistureRamp = smoothstep(0.1, 0.6, moist);

        // final moisture factor
        float humid = moistureGate * moistureRamp;
        // ── combine into probability field
        float snowField = cold * humid;

        // ── sharpen into discrete patches
        // noise shifts threshold → irregular shapes
        float snow = step(0.3, snowField + (n - 0.5) * 0.4);

        // ── slight soft edge to avoid pixel aliasing
        snow = smoothstep(0.45, 0.65, snowField + (n - 0.5) * 0.4);

        vec3 cSnow = vec3(0.87, 0.92, 0.95);

        // ── apply as overlay (not full replacement)
        col = mix(col, cSnow, snow * 0.85);

        // ── slope suppression (snow slides off steep terrain)
        float slope = length(vec2(dFdx(h), dFdy(h))) * 120.0;
        col *= 1.0 - clamp(slope * 0.4, 0.0, 0.35);
    }

    return col;
}

void main() {
    float biome = v_color0.r;
    float moisture = v_color0.g;
    float temp = v_color0.b;
    float h = v_color0.a;
    float tempC = temp * (u_max_temp_c.x - u_min_temp_c.x) + u_min_temp_c.x;

    // wrap only X axis for infinite tiling
    vec2 wp = v_texcoord0.xy;
    wp.x = mod(wp.x, u_world_width.x);

    vec3 col;

    if (h < 0.5) {
        col = waterColor(wp, h, temp, biome, tempC);
    }
    else {
        col  = landColor(wp, h, temp, moisture, biome, tempC);
    }

    // apply features AFTER base color
    col = applySnow(col, wp, h, temp, moisture, tempC);

    // atmospheric haze
    float fog = smoothstep(0.70, 1.0, h) * 0.20;
    col = mix(col, vec3(0.84, 0.88, 0.93), fog);

    gl_FragColor = vec4(col, 1.0);
}