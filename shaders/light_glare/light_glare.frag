$input v_texcoord0

#include <bgfx_shader.sh>

// rgb = glare color times brightness; drawn with additive blending.
uniform vec4 u_glare_color;

void main()
{
    float r2 = dot(v_texcoord0, v_texcoord0);
    float core = exp(-r2 * 24.0);
    float halo = exp(-r2 * 5.0) * 0.3;
    float edge = 1.0 - smoothstep(0.7, 1.0, sqrt(r2));
    gl_FragColor = vec4(u_glare_color.rgb * ((core + halo) * edge), 1.0);
}
