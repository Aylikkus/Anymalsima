$input a_position, a_normal, a_tangent, a_texcoord0, a_texcoord1, a_color0
$output v_position, v_normal, v_layer, v_light, v_color0, v_cover

#include <bgfx_shader.sh>

// xyz = chunk origin - 1 (world tiles), w = 1 when a light volume is bound.
uniform vec4 u_light_chunk;
// xyz = camera eye (world tiles); also the fog parameters of world_surface.sh.
uniform vec4 u_world_eye;

void main()
{
    // The world view is camera-relative: u_model places the mesh relative
    // to the eye (relativeTransform), so adding the eye gives world space for
    // texturing, fog and light while the projection stays precise.
    vec3 world = mul(u_model[0], vec4(a_position, 1.0)).xyz + u_world_eye.xyz;
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    // Move waterfall faces off the terrain wall in the rendered image.
    // Keep the unshifted position for LOD ownership, lighting and flow phase.
    if (a_texcoord1 < -2.5 && abs(a_normal.y) < 0.5)
        gl_Position += mul(u_modelViewProj, vec4(a_normal / 64.0, 0.0));
    v_position = world;
    v_normal = a_normal;
    v_layer = a_texcoord1;
    // Slot-local light texel coordinates, half a block in front of the face
    // (the air cell that lights it). Affine, so interpolation is exact.
    v_light = world - u_light_chunk.xyz + 0.5 * a_normal;
    // Ground cover (ChunkMesher::build): x = overlay layer or -1, y = side
    // fringe range; a_color0 is then the cover tint.
    v_color0 = a_color0;
    // Flowing water uses these two components for its horizontal direction.
    v_cover = a_texcoord0;
}
