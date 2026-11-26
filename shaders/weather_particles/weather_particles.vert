$input a_position, i_data0, i_data1
$output v_uv, v_data

#include <bgfx_shader.sh>

// World-space near precipitation. One instanced quad per drop or flake.
//   i_data0: xyz = centre relative to the eye, w = 0 rain / 1 snow
//   i_data1: xy = half width and half height, z = alpha
uniform vec4 u_weather_right; // camera right in world space
uniform vec4 u_weather_up;    // camera up in world space

void main()
{
    float kind = i_data0.w;
    vec3 side = normalize(u_weather_right.xyz);
    // Rain keeps its streak vertical; snow uses a camera-facing billboard.
    vec3 vertical = kind > 0.5 ? normalize(u_weather_up.xyz)
                               : vec3(0.0, 1.0, 0.0);
    vec3 position = i_data0.xyz
                  + side * (a_position.x * i_data1.x)
                  + vertical * (a_position.y * i_data1.y);
    gl_Position = mul(u_viewProj, vec4(position, 1.0));
    v_uv = a_position.xy;
    v_data = vec2(i_data1.z, kind);
}
