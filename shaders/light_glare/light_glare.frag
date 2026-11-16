$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

// Drawn with additive blending.
void main()
{
    float r2 = dot(v_texcoord0, v_texcoord0);
    float core = exp(-r2 * 24.0);
    float halo = exp(-r2 * 5.0) * 0.3;
    float edge = 1.0 - smoothstep(0.7, 1.0, sqrt(r2));
    gl_FragColor = vec4(v_color0 * ((core + halo) * edge), 1.0);
}
