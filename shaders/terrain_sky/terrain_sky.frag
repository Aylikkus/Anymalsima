$input v_ray

#include <bgfx_shader.sh>
void main()
{
    float elevation = max(normalize(v_ray).y, 0.0);
    vec3 sky = mix(vec3(0.68, 0.77, 0.84), vec3(0.24, 0.47, 0.72),
                   smoothstep(0.0, 0.85, elevation));
    gl_FragColor = vec4(sky, 1.0);
}
