// Shared by voxel terrain and distant terrain. World-space texture mapping
// and the same light/fog response keep the two representations consistent.
//
// Including programs declare u_texture_array and u_normal_array (the normal
// array uses the same layer index as the albedo array), u_shadow_atlas
// (shadow_sampling.sh), u_lod_light_map and u_sky_occlusion.
uniform vec4 u_world_eye;
// x = flow time in seconds, y = eye submerged, z = debug LOD edges.
uniform vec4 u_world_water;
#include "shadow_sampling.sh"
#include "water_fog.sh"

// Periodic value noise keeps the same moving highlights across the X wrap.
float waterHash(vec2 cell)
{
    vec3 p = fract(vec3(mod(cell, 16.0).xyx) * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float waterNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(waterHash(i), waterHash(i + vec2(1.0, 0.0)), f.x),
               mix(waterHash(i + vec2(0.0, 1.0)), waterHash(i + 1.0), f.x), f.y);
}

// Keep transitions narrow on screen without turning a moving band edge into
// a staircase. The cap keeps distant triangles from losing their contrast.
float surfaceBand(float value, float edge)
{
    float width = clamp(fwidth(value), 0.015, 0.085);
    return smoothstep(edge - width, edge + width, value);
}

vec3 surfaceWaterColor(vec3 position, vec3 normal, vec2 flow, bool flowing,
                       float depth, float impact)
{
    float t = u_world_water.x;
    vec3 color = mix(vec3(0.16, 0.47, 0.47), vec3(0.08, 0.30, 0.42), depth);
    if (abs(normal.y) < 0.5 && flowing) {
        // Use long ribbons on falls and break them up as they descend.
        float across = dot(position.xz, vec2(-normal.z, normal.x));
        float down = -position.y - 4.0 * t;
        vec2 p = vec2(across * 2.0, down * 0.25);
        p.x += 0.4 * waterNoise(vec2(across, down * 0.5));
        float ribbon = waterNoise(p);
        color = mix(color, vec3(0.36, 0.63, 0.68), surfaceBand(ribbon, 0.48));
        return mix(color, vec3(0.74, 0.88, 0.86), surfaceBand(ribbon, 0.79));
    }

    // The broad tint survives at a distance. Fade the finer crests as their
    // screen footprint shrinks so ocean and river LODs do not sparkle.
    vec2 p = position.xz - (flowing ? flow * t * 2.5 : vec2(t * 0.12, 0.0));
    float footprint = max(length(dFdx(p)), length(dFdy(p)));
    float broad = waterNoise(p * 0.125);
    float grazing = 1.0 - abs(normalize(u_world_eye.xyz - position).y);
    color += vec3(0.05, 0.09, 0.12) * grazing * grazing;
    color += vec3(0.018, 0.034, 0.043) * surfaceBand(broad, 0.53)
           * (1.0 - smoothstep(1.0, 6.0, footprint));

    // Fixed frequencies keep the curved contours periodic across the wrap
    // (one region is 128 tiles), even when the current is diagonal.
    vec2 bend = vec2_splat(broad * 0.65);
    float wave = flowing
        ? mix(waterNoise(p * vec2(0.875, 0.25) + bend),
              waterNoise(p * vec2(0.25, 0.875) + bend), flow.x * flow.x)
        : waterNoise(p * vec2(0.25, 0.5) + bend);
    float detail = 1.0 - smoothstep(0.25, 1.1, footprint);
    float ridge = 1.0 - abs(wave - 0.63) * 2.0;
    float crest = surfaceBand(ridge, 0.90) * surfaceBand(broad, 0.48);
    color += vec3(0.11, 0.18, 0.18) * crest * detail * (flowing ? 1.0 : 0.40);
    float foam = surfaceBand(wave, 0.70) * impact;
    return mix(color, vec3(0.70, 0.87, 0.84), foam * detail);
}

// Artificial light response: ambient + diffuse * N.L, both scaled by the
// light level.
#define SURFACE_AMBIENT 0.6
#define SURFACE_DIFFUSE 0.4
#define SURFACE_MIN_LIGHT 0.08
// Texture rows are uploaded top-down, so +t points down the image. Normal
// maps authored OpenGL-style (+green = up) need -1 here, DirectX-style +1.
#define NORMAL_GREEN_SIGN -1.0

#include "sky_colors.sh"

vec3 triplanarWeights(vec3 normal)
{
    vec3 weights = pow(abs(normal), vec3(8.0, 8.0, 8.0));
    return weights / max(dot(weights, vec3(1.0, 1.0, 1.0)), 0.0001);
}

vec3 decodeTangentNormal(vec4 texel)
{
    vec3 n = texel.xyz * 2.0 - 1.0;
    n.y *= NORMAL_GREEN_SIGN;
    // Box-filtered mips shorten the vectors; renormalize.
    return n * inversesqrt(max(dot(n, n), 0.000001));
}

// Normal-mapped world normal for the triplanar projection in
// surfaceTexture(). Each projection plane's TBN comes from its texture axes:
//   X plane: uv = pos.zy  -> T = +Z, B = +Y
//   Y plane: uv = pos.xz  -> T = +X, B = +Z
//   Z plane: uv = pos.xy  -> T = +X, B = +Y
// N is the geometric normal, so mirrored (negative) faces stay correct and
// smooth LOD normals blend like a UDN triplanar.
vec3 surfaceNormal(vec3 position, vec3 normal, float layer)
{
    vec3 weights = triplanarWeights(normal);
    vec3 nx = decodeTangentNormal(
        texture2DArray(u_normal_array, vec3(position.zy, layer)));
    vec3 ny = decodeTangentNormal(
        texture2DArray(u_normal_array, vec3(position.xz, layer)));
    vec3 nz = decodeTangentNormal(
        texture2DArray(u_normal_array, vec3(position.xy, layer)));
    vec3 wx = vec3(0.0, nx.y, nx.x) + normal * nx.z;
    vec3 wy = vec3(ny.x, 0.0, ny.y) + normal * ny.z;
    vec3 wz = vec3(nz.x, nz.y, 0.0) + normal * nz.z;
    return normalize(wx * weights.x + wy * weights.y + wz * weights.z);
}

// Far lights (LightRenderer): lights reaching beyond the voxel near field,
// evaluated per pixel. Four vec4 per light:
//   [0] xyz = world position (copy nearest the eye), w = radius
//   [1] rgb = color * intensity, w = falloff exponent
//   [2] xyz = world forward, w = cos(outer angle), or -2 without a cone
//   [3] x = cos(inner angle), y = 1 when the voxel near field covers the
//       first SURFACE_NEAR_RANGE blocks (the term fades in over that band),
//       z = ready shadow slot in the atlas, w = open faces of the emitter
#define SURFACE_MAX_FAR_LIGHTS 32
#define SURFACE_NEAR_RANGE 15.0
uniform vec4 u_lights[128];
// x = far light count.
uniform vec4 u_light_info;
// Distant glow of small lights (LodLightMap): xy = world XZ of the map's
// minimum corner, z = 1 / world extent, w = 1 when the map is valid.
uniform vec4 u_lod_light_origin;

// Small lights on distant terrain: rgb light from the coarse map, faded by
// the height difference to the strongest light there (alpha).
vec3 surfaceLodLight(vec3 position)
{
    if (u_lod_light_origin.w < 0.5) return vec3_splat(0.0);
    vec2 uv = (position.xz - u_lod_light_origin.xy) * u_lod_light_origin.z;
    if (any(lessThan(uv, vec2_splat(0.0))) || any(greaterThan(uv, vec2_splat(1.0))))
        return vec3_splat(0.0);
    vec4 texel = texture2DLod(u_lod_light_map, uv, 0.0);
    float vertical = clamp(1.0 - abs(position.y - texel.a)
                                     / SURFACE_NEAR_RANGE, 0.0, 1.0);
    return texel.rgb * vertical;
}

// Sky light under structures (SkyOcclusionMap, docs/handoff/structures.md)
// for surfaces without a voxel light volume: xy = world XZ of the map's
// minimum corner, z = 1 / world extent, w = 1 when the map is valid. Texels
// (point sampled) hold, for k = 0..3, the lowest occluder top within k
// texels, or SURFACE_OPEN_SKY: a point at height y is k texels from open
// sky for the smallest k whose threshold is at most y, like the voxel
// sunlight (15 - distance to open sky, per tile).
uniform vec4 u_sky_occlusion_origin;
// SkyOcclusionMap::TilesPerTexel, Open; fade below a threshold:
// SkyOcclusionMap::Fade; sky per distance: skyAtDistance(1..4).
#define SURFACE_OCCLUSION_CELL 4.0
#define SURFACE_OPEN_SKY -30000.0
#define SURFACE_SKY_K1 0.833333
#define SURFACE_SKY_K2 0.566667
#define SURFACE_SKY_K3 0.3
#define SURFACE_SKY_K4 0.033333

float surfaceOcclusionTexel(vec2 texel, float y)
{
    vec2 uv = (texel + 0.5) * (SURFACE_OCCLUSION_CELL * u_sky_occlusion_origin.z);
    vec4 t = texture2DLod(u_sky_occlusion, uv, 0.0);
    if (t.x <= SURFACE_OPEN_SKY + 1.0) return 1.0;
    // How far the point is above each threshold (they only fall with k).
    vec4 above = smoothstep(t - 2.0 * SURFACE_OCCLUSION_CELL,
                            t - SURFACE_OCCLUSION_CELL, vec4_splat(y));
    above.y = max(above.y, above.x);
    above.z = max(above.z, above.y);
    above.w = max(above.w, above.z);
    return above.x + (above.y - above.x) * SURFACE_SKY_K1
         + (above.z - above.y) * SURFACE_SKY_K2
         + (above.w - above.z) * SURFACE_SKY_K3
         + (1.0 - above.w) * SURFACE_SKY_K4;
}

// Sky fraction (0-1) at a surface point, sampled half a cell in front of
// the face (the air that lights it, as the voxel light volumes are) and
// blended between the four nearest texels.
float surfaceStructureSky(vec3 position, vec3 normal)
{
    if (u_sky_occlusion_origin.w < 0.5) return 1.0;
    vec3 p = position + normal * (0.5 * SURFACE_OCCLUSION_CELL);
    vec2 uv = (p.xz - u_sky_occlusion_origin.xy) * u_sky_occlusion_origin.z;
    if (any(lessThan(uv, vec2_splat(0.0))) || any(greaterThan(uv, vec2_splat(1.0))))
        return 1.0;
    vec2 at = (p.xz - u_sky_occlusion_origin.xy) / SURFACE_OCCLUSION_CELL - 0.5;
    vec2 base = floor(at);
    vec2 f = at - base;
    float a = surfaceOcclusionTexel(base, p.y);
    float b = surfaceOcclusionTexel(base + vec2(1.0, 0.0), p.y);
    float c = surfaceOcclusionTexel(base + vec2(0.0, 1.0), p.y);
    float d = surfaceOcclusionTexel(base + vec2(1.0, 1.0), p.y);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// nearField: 1 where the voxel light volume covers the first
// SURFACE_NEAR_RANGE blocks (detailed chunks), so the far term fades in
// beyond it; 0 on surfaces without voxel light (distant terrain, joins,
// structures), which take the far term all the way to the light.
vec3 surfaceFarLight(vec3 position, vec3 normal, vec3 geometric, float nearField)
{
    vec3 sum = vec3_splat(0.0);
    int count = int(u_light_info.x + 0.5);
    for (int i = 0; i < SURFACE_MAX_FAR_LIGHTS; ++i) {
        if (i >= count) break;
        vec4 placement = u_lights[i * 4];
        vec4 color = u_lights[i * 4 + 1];
        vec4 cone = u_lights[i * 4 + 2];
        vec4 extra = u_lights[i * 4 + 3];
        vec3 toLight = placement.xyz - position;
        float d = length(toLight);
        if (d >= placement.w) continue;
        vec3 l = toLight / max(d, 0.0001);
        float strength = pow(max(1.0 - d / placement.w, 0.0), color.w);
        if (cone.w > -1.5)
            strength *= smoothstep(cone.w, extra.x, dot(-l, cone.xyz));
        if (extra.y > 0.5 && nearField > 0.5)
            strength *= smoothstep(SURFACE_NEAR_RANGE - 4.0,
                                   SURFACE_NEAR_RANGE, d);
        if (strength <= 0.0) continue;
        // A lamp at the center of its tile reaches the first face along the
        // original ray before any shadow-map bias. A solid tile across that
        // face blocks the ray even if its mesh is missing from the atlas.
        vec3 offset = abs(toLight);
        if (extra.w < 63.0 && max(offset.x, max(offset.y, offset.z)) > 0.5) {
            float face;
            vec2 ndc;
            shadowFace(-toLight, face, ndc);
            if ((int(extra.w) & (1 << int(face))) == 0) continue;
        }
        float visibility = shadowVisibility(extra.z, -toLight, geometric,
                                            placement.w);
        // Retain filtering across the silhouette while sharpening the edge.
        strength *= smoothstep(0.32, 0.68, visibility);
        float facing = SURFACE_AMBIENT
                     + SURFACE_DIFFUSE * max(dot(normal, l), 0.0);
        sum += color.rgb * (strength * facing);
    }
    return sum;
}

// Block light chroma (LightVolume BA channels, BlockLightPropagator::
// encodeChroma) to an RGB tint whose brightest channel is 1.
vec3 surfaceLightTint(vec2 chroma)
{
    float co = (chroma.x * 255.0 - 128.0) / 63.5;
    float cg = (chroma.y * 255.0 - 128.0) / 127.0;
    vec3 rgb = max(vec3(1.0 + co - cg, 1.0 + cg, 1.0 - co - cg),
                   vec3_splat(0.0));
    return rgb / max(max(rgb.r, rgb.g), max(rgb.b, 0.0001));
}

// Use the underlying shape so texture detail and its mipmaps do not break
// up the broad shadow bands. Sky exposure stays continuous in caves, under
// canopies and in water, with the warm bands strongest under open sky.
vec3 surfaceSunLight(vec3 geometric, float exposure)
{
    float facing = dot(geometric, skySunDirection());
    float direct = skyDirectSun() * smoothstep(0.55, 0.95, exposure);
    vec3 shade = vec3(0.49, 0.53, 0.63);
    vec3 ramp = mix(shade, vec3(0.76, 0.77, 0.80), surfaceBand(facing, 0.14));
    ramp = mix(ramp, vec3(1.0, 0.97, 0.91), surfaceBand(facing, 0.60));
    return mix(shade, ramp, direct) * (exposure * skyDaylight());
}

// light.x = sky exposure, light.y = block (artificial) light, both 0..1.
// blockTint colors the block light.
// Block light comes from blockDir (toward brighter space); blockDirStrength
// fades that to non-directional where the light field is flat. far is
// surfaceFarLight() plus surfaceLodLight(). Block light saturates at 1 per
// channel, like the voxel field, so near and far light hand over smoothly.
vec3 surfaceLight(vec3 geometric, vec3 normal, vec3 albedo, vec2 light,
                  vec3 blockTint, vec3 blockDir, float blockDirStrength,
                  vec3 far)
{
    vec3 sky = surfaceSunLight(geometric, light.x);
    float toward = mix(1.0, max(dot(normal, blockDir), 0.0), blockDirStrength);
    vec3 block = blockTint * (light.y
                              * (SURFACE_AMBIENT + SURFACE_DIFFUSE * toward))
               + far;
    vec3 lit = max(sky, min(block, vec3_splat(1.0)));
    return albedo * max(lit, vec3_splat(SURFACE_MIN_LIGHT));
}
float surfaceWaterSky(float y, float waterHeight)
{
    // Water opacity is 8: full sky at the surface, about 7/15 one tile
    // down and zero by two. surfaceLight applies the usual minimum light.
    return waterHeight <= 0.0 ? 1.0 :
        clamp(1.0 - (waterHeight - y) * (8.0 / 15.0), 0.0, 1.0);
}

vec3 surfaceFog(vec3 position, vec3 color)
{
    float distanceToEye = length(position - u_world_eye.xyz);
    if (u_world_water.y > 0.5) {
        float fog = smoothstep(2.0, 48.0, distanceToEye);
        return mix(color, WATER_FOG_COLOR, fog);
    }
    float fog = smoothstep(u_world_eye.w * 0.45, u_world_eye.w, distanceToEye);
    return mix(color, skyHorizon(skySunDirection().y), fog);
}
vec4 surfaceTexture(vec3 position, vec3 normal, float layer)
{
    vec3 weights = triplanarWeights(normal);
    return texture2DArray(u_texture_array, vec3(position.zy, layer)) * weights.x
         + texture2DArray(u_texture_array, vec3(position.xz, layer)) * weights.y
         + texture2DArray(u_texture_array, vec3(position.xy, layer)) * weights.z;
}

// Ground cover (docs/handoff/flora.md), shared by voxel and distant terrain
// so grass looks the same on both sides of the hand-over. The overlay alpha
// orders texels: a texel shows once the density exceeds 1 - alpha, so edges
// are ragged, not tile squares.
float surfaceCoverHash(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

float surfaceCoverNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = surfaceCoverHash(vec3(i, 0.0));
    float b = surfaceCoverHash(vec3(i + vec2(1.0, 0.0), 0.0));
    float c = surfaceCoverHash(vec3(i + vec2(0.0, 1.0), 0.0));
    float d = surfaceCoverHash(vec3(i + vec2(1.0, 1.0), 0.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Coverage of a top face.
float surfaceCoverTop(float density, float alpha)
{
    return smoothstep(-0.05, 0.05, density - (1.0 - alpha));
}

// Coverage of a side face: a fringe hanging `depth` metres below the
// covered tile's top edge. fringe: this tile's length (up to the whole side);
// blade alpha makes the lower edge ragged.
float surfaceCoverSide(float density, float alpha, float fringe, float depth)
{
    float reach = fringe * (0.75 + 0.5 * alpha)
        * clamp(density * 2.0, 0.0, 1.0);
    return 1.0 - smoothstep(reach - 0.02, reach + 0.02, depth);
}

// Paints the overlay of `layer` over albedo and normal, with broad painted
// value variation across the tint (by XZ only, so smooth distant slopes
// show no contour lines).
void surfaceApplyCover(vec3 position, vec3 geometric, float layer,
                       vec4 overlay, float coverage, vec3 coverTint,
                       inout vec3 color, inout vec3 normal)
{
    vec3 tint = coverTint
        * (0.88 + 0.24 * surfaceCoverNoise(position.xz * 0.11));
    color = mix(color, overlay.rgb * tint, coverage);
    normal = normalize(mix(normal, surfaceNormal(position, geometric, layer),
                           coverage));
}
