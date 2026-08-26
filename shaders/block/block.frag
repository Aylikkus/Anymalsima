$input v_texcoord0, v_layer, v_color0

#include <bgfx_shader.sh>

SAMPLER2DARRAY(u_texture_array, 0);

void main()
{
    vec4 tex = texture2DArray(u_texture_array, vec3(v_texcoord0, v_layer));
    gl_FragColor = tex * v_color0;
}