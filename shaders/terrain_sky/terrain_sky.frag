$input v_ray

#include <bgfx_shader.sh>
#include "../water_fog.sh"

uniform vec4 u_sky_camera;

void main()
{
    if (u_sky_camera.w > 0.5) {
        gl_FragColor = vec4(WATER_FOG_COLOR, 1.0);
        return;
    }
    float elevation = max(normalize(v_ray).y, 0.0);
    vec3 sky = mix(vec3(0.68, 0.77, 0.84), vec3(0.24, 0.47, 0.72),
                   smoothstep(0.0, 0.85, elevation));
    gl_FragColor = vec4(sky, 1.0);
}
