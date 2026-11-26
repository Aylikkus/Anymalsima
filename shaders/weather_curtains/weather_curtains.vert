$input a_position, i_data0
$output v_position, v_depth

#include <bgfx_shader.sh>

uniform vec4 u_weather_right;
uniform vec4 u_weather_up;

void main()
{
    // i_data0: slice depth, half width, half height and sample spacing.
    vec3 side = u_weather_right.xyz;
    vec3 up = u_weather_up.xyz;
    vec3 forward = cross(side, up);
    v_position = forward * i_data0.x
               + side * (a_position.x * i_data0.y)
               + up * (a_position.y * i_data0.z);
    v_depth = i_data0.xw;
    gl_Position = mul(u_viewProj, vec4(v_position, 1.0));
}
