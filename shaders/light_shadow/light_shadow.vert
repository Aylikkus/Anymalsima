$input a_position, a_normal
$output v_rel, v_caster

#include <bgfx_shader.sh>

// Shadow caster pass of one cube face (LightShadows). The view is centred on
// the light: u_model places the mesh relative to it, so v_rel is the vertex
// position relative to the light.
void main()
{
    v_rel = mul(u_model[0], vec4(a_position, 1.0)).xyz;
    // Only coarse structure casters are scaled (StructureLod::scale).
    float cellSize = mul(u_model[0], vec4(1.0, 0.0, 0.0, 0.0)).x;
    v_caster = vec4(a_normal, cellSize);
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
}
