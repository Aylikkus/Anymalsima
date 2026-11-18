$input a_position, a_normal, a_texcoord0, a_texcoord1, a_color0, a_color1, i_data0, i_data1, i_data2, i_data3
$output v_position, v_normal, v_uv, v_params, v_color0, v_color1, v_light, v_ground, v_fade, v_tint

#include <bgfx_shader.sh>

// Instanced plants (FloraRenderer, FloraMeshBuilder; FoliageVertex in
// engine/render/vertex.hpp). The world view is camera-relative, so each
// instance holds its base position relative to the eye.
//   i_data0: xyz = base relative to the eye, w = yaw (radians)
//   i_data1: xyz = scale, w = seed in [0, 1)
//   i_data2: rgb = ground colour under the plant, w = withering 0..1
//   i_data3: x = sky light, y = block light, an exact integer
//            (base * 16 + crown) * 65536 + Co * 256 + Cg (foliage_light::pack:
//            4-bit levels near the ground and in the crown, plants use the
//            same level twice; chroma bytes as in light volumes, 128 =
//            white), z = fade distance, w = fade-in distance (0: none; far
//            tree billboards)
// a_texcoord0: uv, texture layer (-1 untextured), height 0 root .. 1 top.
// a_texcoord1: translucency, hue jitter, value jitter, normal mode.
uniform vec4 u_world_eye;  // xyz = eye (world tiles)
// x = time (s), y = wind strength, z = 1: tree crowns (TreeRenderer):
// a_texcoord0.w is then the wind weight (0 trunk .. 1 outer crown) and
// positions span the whole tree, so sway does not grow with height.
uniform vec4 u_foliage;

vec3 yawRotate(vec3 v, float c, float s)
{
    return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

void main()
{
    float c = cos(i_data0.w), s = sin(i_data0.w);
    vec3 local = yawRotate(a_position * i_data1.xyz, c, s);
    // Wind: tips sway most; each plant has its own phase, and a slow gust
    // wave travels across the world.
    float height = a_texcoord0.w;
    vec3 world = i_data0.xyz + u_world_eye.xyz;
    float phase = i_data1.w * 6.2831853 + dot(world.xz, vec2(0.21, 0.17));
    float gust = 0.6 + 0.4 * sin(u_foliage.x * 0.35 + world.x * 0.03
                                 + world.z * 0.02);
    vec2 sway = vec2(sin(u_foliage.x * 1.7 + phase),
                     cos(u_foliage.x * 1.3 + phase * 1.3));
    float amplitude = u_foliage.z > 0.5
        ? 0.3 * height
        : height * height * (0.04 + 0.05 * max(a_position.y, 0.0));
    local.xz += sway * (u_foliage.y * gust * amplitude);
    vec3 rel = i_data0.xyz + local;
    gl_Position = mul(u_viewProj, vec4(rel, 1.0));
    v_position = rel + u_world_eye.xyz;
    v_normal = yawRotate(a_normal / max(i_data1.xyz, vec3_splat(0.01)), c, s);
    v_uv = a_texcoord0;
    v_params = a_texcoord1;
    v_color0 = a_color0;
    v_color1 = a_color1;
    // Decoded here, not per fragment: interpolation would not keep the
    // packed integer exact.
    float packed = floor(i_data3.y + 0.5);
    float levels = floor(packed / 65536.0);
    float chroma = packed - levels * 65536.0;
    float co = floor(chroma / 256.0);
    float base = floor(levels / 16.0);
    v_light = vec4(i_data3.x, base / 15.0, i_data2.w, i_data1.w);
    // xy = block light chroma (0..1 bytes), z = crown block light.
    v_tint = vec4(co / 255.0, (chroma - co * 256.0) / 255.0,
                  (levels - base * 16.0) / 15.0, 0.0);
    v_ground = vec4(i_data2.rgb, i_data3.z);
    // y: world height of the plant's base (tree block light by height).
    v_fade = vec4(i_data3.w, i_data0.y + u_world_eye.y, 0.0, 0.0);
}
