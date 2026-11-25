$input v_position, v_normal, v_layer, v_light, v_color0, v_cover, v_owner

#include <bgfx_shader.sh>
SAMPLER2DARRAY(u_texture_array, 0);
SAMPLER2DARRAY(u_normal_array, 1);
// Paged atlas of 18^3 chunk light volumes (LightVolumeManager):
// r = sky light, g = block light, ba = block light chroma. Each slot has a one-cell border copied
// from neighboring chunks, so faces on chunk borders filter seamlessly.
SAMPLER3D(u_light_volume, 2);
// w = 0: no volume, use full sky light; 1: swept voxel light; 2: estimated
// sky light (SunLightPropagator::isEstimated), shaded by u_sky_occlusion.
uniform vec4 u_light_chunk;
uniform vec4 u_light_slot;   // xyz = slot base texel inside the page
// LightShadows atlas and LodLightMap (LightRenderer).
SAMPLER2D(u_shadow_atlas, 3);
SAMPLER2D(u_lod_light_map, 4);
// Ground-cover density in the light volume's slot layout (R, 0-1).
SAMPLER3D(u_cover_volume, 5);
// Sky light under structures (SkyOcclusionMap, LightRenderer), for draws
// without a light volume.
SAMPLER2D(u_sky_occlusion, 6);
#include "../world_surface.sh"

// 1 / (144, 144, 72): LightVolumeManager::PageX/PageY/PageZ.
#define LIGHT_PAGE_INV vec3(1.0 / 144.0, 1.0 / 144.0, 1.0 / 72.0)
// Texel centres that stay inside the slot, border included.
#define LIGHT_MIN 0.5
#define LIGHT_MAX 17.5

vec4 sampleLightVolume(vec3 local)
{
    vec3 texel = clamp(local, vec3_splat(LIGHT_MIN), vec3_splat(LIGHT_MAX));
    return texture3D(u_light_volume, (texel + u_light_slot.xyz)
                                         * LIGHT_PAGE_INV);
}
vec2 sampleLight(vec3 local)
{
    return sampleLightVolume(local).rg;
}

// Paints the ground-cover overlay (docs/handoff/flora.md, world_surface.sh)
// over a face's soil albedo and normal; side faces get a fringe.
void applyCover(vec3 position, vec3 lightCell, vec4 owner, vec2 cover,
                vec3 coverTint, vec3 geometric, inout vec3 color,
                inout vec3 normal)
{
    // Density lives in the covered tile's cell: step back from the lit air
    // cell, and use the cell centre vertically (bilinear across tiles only
    // along the face). The small offset keeps the quads' seam overlap at a
    // side face's top edge in the tile below it.
    bool explicitOwner = owner.w >= 0.0;
    vec3 cell = explicitOwner ? owner.xyz : lightCell - geometric;
    if (!explicitOwner)
        cell.y = floor(cell.y - 0.003) + 0.5;
    float density = u_light_chunk.w > 0.5
        ? texture3D(u_cover_volume, (clamp(cell, vec3_splat(LIGHT_MIN),
                                           vec3_splat(LIGHT_MAX))
                                     + u_light_slot.xyz) * LIGHT_PAGE_INV).r
        : 0.0;
    float layer = floor(cover.x + 0.5);
    // Project the top coat from above so its pattern joins across a
    // junction crease. The geometric normal still drives its shading.
    bool top = geometric.y > 0.0;
    vec4 overlay = surfaceTexture(
        position, top ? vec3(0.0, 1.0, 0.0) : geometric, layer);
    float coverage;
    if (top) {
        coverage = surfaceCoverTop(density, overlay.a);
    } else {
        float maxFringe = floor(cover.y / 1000.0);
        float minFringe = (cover.y - maxFringe * 1000.0) / 100.0;
        maxFringe /= 100.0;
        float tileY = floor(position.y - 0.5 * geometric.y - 0.003);
        vec3 tile = explicitOwner
            ? owner.xyz - 0.5 + u_light_chunk.xyz
            : vec3(floor(position.x - 0.5 * geometric.x), tileY,
                   floor(position.z - 0.5 * geometric.z));
        float fringe = mix(minFringe, maxFringe, surfaceCoverHash(tile));
        float depth = explicitOwner ? owner.w : tileY + 1.0 - position.y;
        coverage = surfaceCoverSide(density, overlay.a, fringe,
                                    depth);
    }
    surfaceApplyCover(position, geometric, layer, overlay, coverage,
                      coverTint, color, normal);
}

void main()
{
    if (v_layer >= -1.5 && u_world_water.z > 0.5 && u_light_chunk.w < 0.5 &&
        v_cover.x < -0.5 && v_cover.y > 2.5) {
        gl_FragColor = vec4(1.0, 0.5, 0.0, 1.0);
        return;
    }
    vec3 geometric = normalize(v_normal);
    vec3 color;
    vec3 normal = geometric;
    if (v_layer < -1.5) {
        color = surfaceWaterColor(v_position, geometric, v_cover,
                                  v_layer < -2.5, v_color0.a, 1.0 - v_color0.r);
    } else {
        float layer = floor(v_layer + 0.5);
        color = surfaceTexture(v_position, geometric, layer).rgb;
        normal = surfaceNormal(v_position, geometric, layer);
        // Without cover the vertex colour is a plain multiplier; with
        // cover it is the cover tint.
        if (v_cover.x > -0.5)
            applyCover(v_position, v_light, v_owner, v_cover, v_color0.rgb,
                       geometric, color, normal);
        else
            color *= v_color0.rgb;
    }
    vec2 light = vec2(1.0, 0.0);
    vec3 tint = vec3_splat(1.0);
    vec3 blockDir = geometric;
    float blockDirStrength = 0.0;
    if (u_light_chunk.w > 0.5) {
        vec4 cell = sampleLightVolume(v_light);
        light = cell.rg;
        // Sky slabs above are still generating: the estimate ignores the
        // structures above, which the occlusion map knows.
        if (u_light_chunk.w > 1.5)
            light.x *= surfaceStructureSky(v_position, geometric);
        tint = surfaceLightTint(cell.ba);
        // The block-light gradient points toward brighter space, which
        // gives artificial light a direction for the normal maps.
        const float h = 0.5;
        vec3 grad = vec3(
            sampleLight(v_light + vec3(h, 0.0, 0.0)).g
                - sampleLight(v_light - vec3(h, 0.0, 0.0)).g,
            sampleLight(v_light + vec3(0.0, h, 0.0)).g
                - sampleLight(v_light - vec3(0.0, h, 0.0)).g,
            sampleLight(v_light + vec3(0.0, 0.0, h)).g
                - sampleLight(v_light - vec3(0.0, 0.0, h)).g);
        float slope = length(grad);
        // One light level per block (1/15) counts as fully directional.
        blockDirStrength = clamp(slope * 15.0, 0.0, 1.0);
        blockDir = slope > 0.0001 ? grad / slope : geometric;
    } else {
        // Distant joins and structure slabs: the ocean's attenuation and
        // the shade of structures above.
        light.x = surfaceWaterSky(v_position.y, v_layer < -1.5 ? v_position.y : -2.0 - v_cover.x)
                * surfaceStructureSky(v_position, geometric);
    }
    vec3 far = surfaceFarLight(v_position, normal, geometric, u_light_chunk.w);
    // Without a volume (distant joins and structures) small lights come
    // from the coarse distant glow map.
    if (u_light_chunk.w < 0.5) far += surfaceLodLight(v_position);
    color = surfaceLight(geometric, normal, color, light, tint, blockDir,
                         blockDirStrength, far);
    gl_FragColor = vec4(surfaceFog(v_position, color), 1.0);
}
