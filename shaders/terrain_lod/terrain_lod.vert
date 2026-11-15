$input a_position, a_normal, a_tangent, a_texcoord0, a_texcoord1, a_color0
$output v_position, v_normal, v_layer, v_color0

#include <bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_position = a_position;
    v_normal = a_normal;
    v_layer = a_texcoord1;
    v_color0 = a_color0;
}
