$input v_position, v_normal, v_layer, v_color0

#include <bgfx_shader.sh>
SAMPLER2DARRAY(u_texture_array, 0);
SAMPLER2D(u_surface_map, 1);
SAMPLER2D(u_detail_map, 2);
SAMPLER2DARRAY(u_normal_array, 3);
uniform vec4 u_lod_patch;
uniform vec4 u_detail_origin;
#include "../world_surface.sh"

void main()
{
    vec2 owner = v_position.xz;
    // One centimeter remains representable at the far edge of a 64 km world.
    if (u_lod_patch.w < 1.5 || abs(v_normal.y) < 0.01)
        owner -= v_normal.xz * 0.01;
    vec2 detailUv = (owner - u_detail_origin.xy) / u_detail_origin.z;
    if (all(greaterThanEqual(detailUv, vec2(0.0)))
        && all(lessThan(detailUv, vec2(1.0)))) {
        if (texture2D(u_detail_map, detailUv).r > 0.5) discard;
    }
    vec3 normal = normalize(v_normal);
    vec3 color;
    vec3 surface = normal;
    if (v_layer < -1.5) {
        color = vec3(0.055, 0.24, 0.32);
    } else {
        float layer = floor(v_layer + 0.5);
        if (v_layer < -0.5) {
            vec2 uv = (v_position.xz - u_lod_patch.xy) / u_lod_patch.z;
            vec4 encoded = floor(texture2D(u_surface_map, uv) * 255.0 + 0.5);
            layer = normal.y > 0.65 ? encoded.r + encoded.g * 256.0
                                    : encoded.b + encoded.a * 256.0;
        }
        color = surfaceTexture(v_position, normal, layer).rgb;
        surface = surfaceNormal(v_position, normal, layer);
    }
    // Distant terrain is fully sky-lit and has no block light.
    color = surfaceLight(surface, color, vec2(1.0, 0.0), normal, 0.0);
    gl_FragColor = vec4(surfaceFog(v_position, color), 1.0);
}
