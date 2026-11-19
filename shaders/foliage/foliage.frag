$input v_position, v_normal, v_uv, v_params, v_color0, v_color1, v_light, v_ground, v_fade, v_tint

#include <bgfx_shader.sh>
// Foliage cards (FoliageMaterial albedo/normal, layer = material id - 1).
SAMPLER2DARRAY(u_texture_array, 0);
SAMPLER2DARRAY(u_normal_array, 1);
// LightShadows atlas and LodLightMap (LightRenderer, block stages).
SAMPLER2D(u_shadow_atlas, 3);
SAMPLER2D(u_lod_light_map, 4);
// Sky occlusion under structures (unused: foliage sky light comes from the
// CPU, foliage_light).
SAMPLER2D(u_sky_occlusion, 6);
#include "../world_surface.sh"
uniform vec4 u_foliage;  // x = time, y = wind, z = 1: tree crowns

// Rotates a colour's hue around the grey axis.
vec3 hueShift(vec3 color, float angle)
{
    const vec3 k = vec3(0.57735, 0.57735, 0.57735);
    float c = cos(angle);
    return color * c + cross(k, color) * sin(angle)
         + k * dot(k, color) * (1.0 - c);
}

void main()
{
    // Distance fade: dithered, so plants thin out instead of popping.
    float distanceToEye = length(v_position - u_world_eye.xyz);
    float fadeEnd = v_ground.w;
    float fade = 1.0 - smoothstep(fadeEnd * 0.8, fadeEnd, distanceToEye);
    float dither = surfaceCoverHash(vec3(floor(gl_FragCoord.xy), 7.0));
    if (dither > fade) discard;
    // Fade-in (billboards): the complement of a mesh fading out over the
    // same distances, so the two cross-fade without gaps or overlap.
    float fadeIn = v_fade.x;
    if (fadeIn > 0.0 &&
        dither <= 1.0 - smoothstep(fadeIn * 0.8, fadeIn, distanceToEye))
        discard;

    float layer = v_uv.z;
    vec4 texel = vec4(0.85, 0.85, 0.85, 1.0);
    if (layer > -0.5)
        texel = texture2DArray(u_texture_array, vec3(v_uv.xy, floor(layer + 0.5)));
    if (texel.a < 0.5) discard;

    float height = v_uv.w;
    float seed = v_light.w;
    vec3 ground = v_ground.rgb;
    // Palette colour (gradient baked per vertex) times the card's detail.
    vec3 color = v_color0.rgb * texel.rgb * 1.1;
    vec3 dry = v_color1.rgb * texel.rgb * 1.1;
    // Per-plant variation from the seed.
    color = hueShift(color, (seed * 2.0 - 1.0) * v_params.y * 6.2831853);
    color *= 1.0 + (fract(seed * 7.13) * 2.0 - 1.0) * v_params.z;
    color = mix(color, dry, v_light.z);
    // Lean toward the ground's hue (plants and grass share a hue family),
    // and darken toward the ground colour at the root.
    float luma = dot(color, vec3(0.3, 0.59, 0.11));
    vec3 groundHue = ground / max(dot(ground, vec3(0.3, 0.59, 0.11)), 0.05)
                   * luma;
    color = mix(color, groundHue, 0.12);
    // (Plants only: in tree crowns height is the wind weight.)
    if (u_foliage.z < 0.5)
        color = mix(ground * 0.7, color, smoothstep(0.0, 0.3, height));

    vec3 normal = normalize(v_normal);
    vec3 toEye = normalize(u_world_eye.xyz - v_position);
    // Cards light from the side facing the viewer.
    if (v_params.w < 0.5 && dot(normal, toEye) < 0.0) normal = -normal;
    vec2 light = v_light.xy;
    if (u_foliage.z > 0.5) {
        // Tree crowns: block light near the ground (y) and in the crown
        // (v_tint.z), blended up the tree, so a lamp at its foot lights the
        // trunk and lower branches, not the whole tree alike.
        light.y = mix(light.y, v_tint.z,
                      smoothstep(1.5, 6.0, v_position.y - v_fade.y));
    }
    // Near-field block light takes the voxel field's colour, like blocks.
    vec3 tint = surfaceLightTint(v_tint.xy);
    vec3 far = surfaceFarLight(v_position, normal, normal, light.x, 1.0);
    vec3 lit = surfaceLight(normal, color, light, tint, normal, 0.0, far);
    // Translucency: sunlight through leaves seen against the sun.
    float daylight = 1.0 - clamp(u_light_info.y, 0.0, 1.0);
    float back = max(dot(-toEye, surfaceSunDirection()), 0.0);
    lit += color * v_params.x * light.x * daylight
         * (0.15 + 0.85 * back * back) * 0.3;
    gl_FragColor = vec4(surfaceFog(v_position, lit), 1.0);
}
