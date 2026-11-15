$input v_position, v_normal, v_layer, v_color0

#include <bgfx_shader.sh>
SAMPLER2DARRAY(u_texture_array, 0);
#include "../world_surface.sh"

void main()
{
    vec3 color;
    if (v_layer < -1.5) {
        color = vec3(0.055, 0.24, 0.32);
    } else {
        color = surfaceTexture(v_position, normalize(v_normal), floor(v_layer+0.5)).rgb;
    }
    color = surfaceLight(v_normal, color, v_color0.r);
    gl_FragColor = vec4(surfaceFog(v_position, color), 1.0);
}
