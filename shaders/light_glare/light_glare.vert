$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>

// Camera-facing glare quad of a far light (LightRenderer). a_position.xy
// holds the quad corner in [-1, 1].
// xyz = light centre relative to the eye (the world view is camera-relative),
// w = half size in world tiles.
uniform vec4 u_glare_center;

void main()
{
    vec3 center = mul(u_view, vec4(u_glare_center.xyz, 1.0)).xyz;
    vec3 corner = center + vec3(a_position.xy * u_glare_center.w, 0.0);
    gl_Position = mul(u_proj, vec4(corner, 1.0));
    v_texcoord0 = a_position.xy;
}
