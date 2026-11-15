$input a_position, a_normal, a_tangent, a_texcoord0, a_texcoord1, a_color0
$output v_position, v_normal, v_layer, v_light, v_color0

#include <bgfx_shader.sh>

// xyz = chunk origin - 1 (world tiles), w = 1 when a light volume is bound.
uniform vec4 u_light_chunk;

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_position = a_position;
    v_normal = a_normal;
    v_layer = a_texcoord1;
    // Slot-local light texel coordinates, half a block in front of the face
    // (the air cell that lights it). Affine, so interpolation is exact.
    v_light = a_position - u_light_chunk.xyz + 0.5 * a_normal;
    v_color0 = a_color0;
}
