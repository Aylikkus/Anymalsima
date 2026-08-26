$input v_color0, v_texcoord0
#include <bgfx_shader.sh>

uniform vec4 u_uvRect;
uniform vec4 u_color;
SAMPLER2D(u_tex, 0);

void main()
{
    vec2 uv = u_uvRect.xy + v_texcoord0 * u_uvRect.zw;
    vec4 tex = texture2D(u_tex, uv);
    gl_FragColor = tex * v_color0 * u_color;
}