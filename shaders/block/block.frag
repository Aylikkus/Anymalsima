$input v_position, v_normal, v_layer, v_light, v_color0

#include <bgfx_shader.sh>
SAMPLER2DARRAY(u_texture_array, 0);
SAMPLER2DARRAY(u_normal_array, 1);
// Paged atlas of 18^3 chunk light volumes (LightVolumeManager):
// r = sky light, g = block light. Each slot has a one-cell border copied
// from neighboring chunks, so faces on chunk borders filter seamlessly.
SAMPLER3D(u_light_volume, 2);
uniform vec4 u_light_chunk;  // w = 0: no volume, use full sky light
uniform vec4 u_light_slot;   // xyz = slot base texel inside the page
#include "../world_surface.sh"

// 1 / (144, 144, 72): LightVolumeManager::PageX/PageY/PageZ.
#define LIGHT_PAGE_INV vec3(1.0 / 144.0, 1.0 / 144.0, 1.0 / 72.0)
// Texel centres that stay inside the slot, border included.
#define LIGHT_MIN 0.5
#define LIGHT_MAX 17.5

vec2 sampleLight(vec3 local)
{
    vec3 texel = clamp(local, vec3_splat(LIGHT_MIN), vec3_splat(LIGHT_MAX));
    return texture3D(u_light_volume, (texel + u_light_slot.xyz)
                                         * LIGHT_PAGE_INV).rg;
}

void main()
{
    vec3 geometric = normalize(v_normal);
    vec3 color;
    vec3 normal = geometric;
    if (v_layer < -1.5) {
        color = vec3(0.055, 0.24, 0.32);
    } else {
        float layer = floor(v_layer + 0.5);
        color = surfaceTexture(v_position, geometric, layer).rgb;
        normal = surfaceNormal(v_position, geometric, layer);
    }
    vec2 light = vec2(1.0, 0.0);
    vec3 blockDir = geometric;
    float blockDirStrength = 0.0;
    if (u_light_chunk.w > 0.5) {
        light = sampleLight(v_light);
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
    }
    color = surfaceLight(normal, color * v_color0.rgb, light, blockDir,
                         blockDirStrength);
    gl_FragColor = vec4(surfaceFog(v_position, color), 1.0);
}
