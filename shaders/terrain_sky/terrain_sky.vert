$input a_position
$output v_ray

#include <bgfx_shader.sh>
uniform vec4 u_sky_camera; // yaw, pitch, aspect, underwater

void main()
{
    vec2 ndc = a_position.xy * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    float yaw = u_sky_camera.x;
    float pitch = u_sky_camera.y;
    vec3 forward = vec3(cos(pitch)*sin(yaw), sin(pitch), cos(pitch)*cos(yaw));
    vec3 right = vec3(cos(yaw), 0.0, -sin(yaw));
    vec3 up = cross(forward, right);
    v_ray = forward + right * ndc.x * 0.57735027 * u_sky_camera.z
                    + up * ndc.y * 0.57735027;
}
