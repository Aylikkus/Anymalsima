$input a_position, a_normal, a_tangent, a_texcoord0, a_texcoord1, a_color0
$output v_position, v_normal, v_layer, v_color0, v_cover

#include <bgfx_shader.sh>

// x = offset (0 or +-world width) placing this patch at its copy nearest the
// eye when the world wraps on X. Added exactly, so v_position keeps the
// precision of the stored absolute vertex.
uniform vec4 u_lod_shift;

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_position = a_position + vec3(u_lod_shift.x, 0.0, 0.0);
    v_normal = a_normal;
    v_layer = a_texcoord1;
    v_color0 = a_color0;
    // x: column top above a stepped wall (ground-cover fringe), else -1.
    v_cover = a_texcoord0;
}
