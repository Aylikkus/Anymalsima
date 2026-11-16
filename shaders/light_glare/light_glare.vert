$input a_position, i_data0, i_data1, i_data2
$output v_texcoord0, v_color0

#include <bgfx_shader.sh>

// Camera-facing glare quads of lights (LightRenderer), one instance each.
// a_position.xy holds the quad corner in [-1, 1].
//   i_data0: xyz = glare centre relative to the eye (the world view is
//            camera-relative), w = half size in world tiles
//   i_data1: rgb = color times brightness
//   i_data2: x = shadow slot or -1, y = radius, z = how far the centre was
//            pulled toward the eye
SAMPLER2D(u_shadow_atlas, 0);
#include "../shadow_sampling.sh"

void main()
{
    vec3 color = i_data1.rgb;
    if (i_data2.x > -0.5) {
        // A caster closer to the light than the glare's pulled centre walls
        // the light in: no glare through its enclosure.
        float blocked = shadowCasterDistance(i_data2.x, -i_data0.xyz,
                                             i_data2.y);
        color *= smoothstep(i_data2.z + 0.5, i_data2.z + 2.0, blocked);
    }
    vec3 center = mul(u_view, vec4(i_data0.xyz, 1.0)).xyz;
    vec3 corner = center + vec3(a_position.xy * i_data0.w, 0.0);
    gl_Position = mul(u_proj, vec4(corner, 1.0));
    v_texcoord0 = a_position.xy;
    v_color0 = color;
}
