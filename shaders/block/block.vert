$input a_position, a_normal, a_tangent, a_texcoord0, a_texcoord1, a_color0
$output v_position, v_normal, v_layer, v_light, v_color0, v_cover, v_owner

#include <bgfx_shader.sh>
#include "../river_wave.sh"

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
    vec3 vertex = a_position;
    if (a_texcoord1 < -2.5 && a_texcoord1 > -3.5)
        vertex.y += riverRise(world.xz);
    gl_Position = mul(u_modelViewProj, vec4(vertex, 1.0));
    v_position = world;
    v_normal = a_normal;
    v_layer = a_texcoord1;
    // Slot-local light texel coordinates, half a block in front of the face
    // (the air cell that lights it). Affine, so interpolation is exact.
    v_light = world - u_light_chunk.xyz + 0.5 * a_normal;
    v_owner = vec4(0.0, 0.0, 0.0, -1.0);
    // Explicit faces (including clipped cubes) carry their owning cell.
    // Decode Chunk::cell in a 16-cubed slab and add the halo and half texel.
    if (a_tangent.x >= 2.0 && u_light_chunk.w > 0.5) {
        float index = a_tangent.x - 2.0; // BlockVertex::ExplicitCellTag
        float z = floor(index / 256.0);
        float y = floor(index / 16.0) - 16.0 * z;
        float x = index - 16.0 * y - 256.0 * z;
        vec3 owner = vec3(x, y, z) + 1.5;
        v_owner = vec4(owner, a_tangent.y);

        // Until light transport accounts for partial solids, use the
        // adjacent voxel in the outward direction. On inclined tops that
        // is always above. Keep bilinear filtering along the other axes.
        vec3 axis = abs(a_normal.y) > 0.0
            ? vec3(0.0, sign(a_normal.y), 0.0)
            : sign(a_normal);
        v_light = mix(a_position + 1.0, owner + axis, abs(axis));
    }
    // Ground cover (ChunkMesher::build): x = overlay layer or -1, y = side
    // fringe range; a_color0 is then the cover tint.
    v_color0 = a_color0;
    // Flowing water uses these two components for its horizontal direction.
    v_cover = a_texcoord0;
}
