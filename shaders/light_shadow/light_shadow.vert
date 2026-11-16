$input a_position
$output v_rel

#include <bgfx_shader.sh>

// Shadow caster pass of one cube face (LightShadows). The view is centred on
// the light: u_model places the mesh relative to it, so v_rel is the vertex
// position relative to the light.
void main()
{
    v_rel = mul(u_model[0], vec4(a_position, 1.0)).xyz;
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
}
