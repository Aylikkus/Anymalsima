$input a_position, a_texcoord0, a_texcoord1  // a_texcoord1 contains packed vec4 (biome, moisture, temperature, height)

$output v_color0, v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));  // Transform position

    // Pass attributes to fragment shader
    v_color0 = a_texcoord1;  // Packed vec4 (biome, moisture, temperature, height)
    v_texcoord0 = a_texcoord0;  // World UVs
}