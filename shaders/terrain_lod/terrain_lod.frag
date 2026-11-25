$input v_position, v_normal, v_layer, v_color0, v_cover

#include <bgfx_shader.sh>
SAMPLER2DARRAY(u_texture_array, 0);
SAMPLER2D(u_surface_map, 1);
SAMPLER2D(u_detail_map, 2);
SAMPLER2DARRAY(u_normal_array, 3);
// LightShadows atlas and LodLightMap (LightRenderer).
SAMPLER2D(u_shadow_atlas, 4);
SAMPLER2D(u_lod_light_map, 5);
// Ground cover per texel (TerrainLod::Mesh::covers): r = overlay layer + 1
// (0 = none), g = density, ba = tint RGB565 little endian. Point sampled.
SAMPLER2D(u_cover_map, 6);
// Sky light under structures (SkyOcclusionMap, LightRenderer).
SAMPLER2D(u_sky_occlusion, 7);
SAMPLER2D(u_patch_water, 8);
uniform vec4 u_lod_patch;
uniform vec4 u_lod_cover;  // x = cover texels per side
uniform vec4 u_detail_origin;
#include "../world_surface.sh"

struct CoverSample {
    float layer;
    float density;
    vec3 tint;
};

CoverSample coverTexel(vec2 texel, float size)
{
    vec4 t = texture2D(u_cover_map,
                       (clamp(texel, vec2_splat(0.0), vec2_splat(size - 1.0))
                        + 0.5) / size);
    vec4 b = floor(t * 255.0 + 0.5);
    float rgb = b.b + b.a * 256.0;
    CoverSample s;
    s.layer = b.r - 1.0;
    s.density = t.g;
    s.tint = vec3(floor(rgb / 2048.0) / 31.0,
                  mod(floor(rgb / 32.0), 64.0) / 63.0,
                  mod(rgb, 32.0) / 31.0);
    return s;
}

// The cover at owner: layer from the nearest texel; density and tint
// filtered bilinearly over neighbouring texels of the same cover, like the
// voxel density volume.
CoverSample sampleCover(vec2 owner)
{
    float size = u_lod_cover.x;
    vec2 at = (owner - u_lod_patch.xy) / u_lod_patch.z * size;
    CoverSample nearest = coverTexel(floor(at), size);
    if (nearest.layer < 0.0) return nearest;
    vec2 base = floor(at - 0.5);
    vec2 f = at - 0.5 - base;
    float density = 0.0;
    vec3 tint = vec3_splat(0.0);
    float weights = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 o = vec2(float(i - (i / 2) * 2), float(i / 2));
        CoverSample s = coverTexel(base + o, size);
        float w = (o.x > 0.5 ? f.x : 1.0 - f.x) * (o.y > 0.5 ? f.y : 1.0 - f.y);
        if (abs(s.layer - nearest.layer) > 0.5) continue;
        density += w * s.density;
        tint += w * s.tint;
        weights += w;
    }
    nearest.density = density;
    nearest.tint = weights > 0.0001 ? tint / weights : nearest.tint;
    return nearest;
}

void main()
{
    vec2 owner = v_position.xz;
    // One centimeter remains representable at the far edge of a 64 km world.
    if (u_lod_patch.w < 1.5 || abs(v_normal.y) < 0.01)
        owner -= v_normal.xz * 0.01;
    vec2 detailUv = (owner - u_detail_origin.xy) / u_detail_origin.z;
    if (all(greaterThanEqual(detailUv, vec2(0.0)))
        && all(lessThan(detailUv, vec2(1.0)))) {
        vec2 coverage = texture2D(u_detail_map, detailUv).rg;
        if ((v_layer < -1.5 ? coverage.g : coverage.r) > 0.5) discard;
    }
    if (v_layer >= -1.5 && u_world_water.z > 0.5 && v_cover.y > 0.5) {
        gl_FragColor = v_cover.y < 1.5 ? vec4(1.0, 0.0, 1.0, 1.0)
                                       : vec4(0.0, 1.0, 1.0, 1.0);
        return;
    }
    // Skirts are vertical only to close a gap. Sample the owning patch's
    // top material and light it like the ground, not an exposed rock face.
    bool skirt = v_layer >= -1.5 && v_cover.y > 0.5 && v_cover.y < 1.5;
    vec3 normal = skirt ? vec3(0.0, 1.0, 0.0) : normalize(v_normal);
    vec3 color;
    vec3 surface = normal;
    if (v_layer < -1.5) {
        color = surfaceWaterColor(v_position, normal, v_cover,
                                  v_layer < -2.5, v_color0.a, 1.0 - v_color0.r);
    } else {
        float layer = floor(v_layer + 0.5);
        if (v_layer < -0.5) {
            vec2 uv = ((skirt ? owner : v_position.xz) - u_lod_patch.xy)
                      / u_lod_patch.z;
            vec4 encoded = floor(texture2D(u_surface_map, uv) * 255.0 + 0.5);
            layer = normal.y > 0.65 ? encoded.r + encoded.g * 256.0
                                    : encoded.b + encoded.a * 256.0;
        }
        color = surfaceTexture(v_position, normal, layer).rgb;
        surface = surfaceNormal(v_position, normal, layer);
        // Ground cover on tops, and the fringe on stepped walls (v_cover.x:
        // the column top). Distant walls use one full-height fringe length.
        bool top = normal.y > 0.65;
        bool wall = abs(normal.y) < 0.01 && v_cover.x > -0.5;
        if (top || wall) {
            CoverSample cover = sampleCover(owner);
            if (cover.layer >= 0.0) {
                vec4 overlay = surfaceTexture(v_position, normal, cover.layer);
                float coverage = top
                    ? surfaceCoverTop(cover.density, overlay.a)
                    : surfaceCoverSide(
                          coverTexel(floor((owner - u_lod_patch.xy)
                                           / u_lod_patch.z * u_lod_cover.x),
                                     u_lod_cover.x).density,
                          overlay.a, 0.9, v_cover.x - v_position.y);
                surfaceApplyCover(v_position, normal, cover.layer, overlay,
                                  coverage, cover.tint, color, surface);
            }
        }
    }
    // Without a light volume, approximate the ocean's skylight attenuation.
    // Keep artificial lights independent of this estimate.
    vec2 waterUv = (owner - u_lod_patch.xy) / u_lod_patch.z;
    vec2 encoded = floor(texture2D(u_patch_water, waterUv).rg * 255.0 + 0.5);
    float level = v_layer < -1.5 ? v_position.y : encoded.x + 256.0 * encoded.y;
    float sky = surfaceWaterSky(v_position.y, level)
              * surfaceStructureSky(v_position, normal);
    vec3 far = surfaceFarLight(v_position, surface, normal, 0.0)
             + surfaceLodLight(v_position);
    color = surfaceLight(normal, surface, color, vec2(sky, 0.0),
                         vec3_splat(1.0), normal, 0.0, far);
    gl_FragColor = vec4(surfaceFog(v_position, color), 1.0);
}
