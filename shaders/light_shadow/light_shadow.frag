$input v_rel, v_caster

#include <bgfx_shader.sh>

// Distant terrain coverage of detailed chunk columns, as in terrain_lod.
SAMPLER2D(u_detail_map, 0);
uniform vec4 u_detail_origin;
// x = 1 / light radius, y = 1 for distant terrain (skip detailed columns,
// whose voxel meshes cast instead).
uniform vec4 u_shadow_caster;
// xyz = light position in world tiles (copy nearest the camera).
uniform vec4 u_shadow_light;
// Distant terrain patch being drawn: w = vertex spacing in tiles.
uniform vec4 u_lod_patch;

void main()
{
    // The light's own tile (a lamp block is a cube around its light) never
    // occludes it. Meshes grow every quad by ChunkMesher::SeamOverlap
    // (1/512), so the lamp's faces reach 0.5 + 1/512; anything beyond stays,
    // so neighbor faces that start at the lamp's edges are kept whole (a
    // wider margin let light leak along those edges in thin sheets).
    float cellSize = v_caster.w;
    float seam = cellSize / 512.0 + 0.0002;
    if (cellSize > 1.5) {
        // A coarse branch can grow into the cell occupied by an exposed
        // lamp. Work relative to that cell's centre so the bounds follow
        // the voxel grid even when the lamp is off-centre.
        vec3 centre = 0.5 * cellSize - mod(u_shadow_light.xyz, cellSize);
        vec3 a = abs(v_rel - centre);
        // A face of the containing cell points away from the lamp.
        // An adjacent cell's face on the same plane points toward it.
        if (dot(v_rel, v_caster.xyz) > 0.0 &&
            max(a.x, max(a.y, a.z)) < 0.5 * cellSize + seam) discard;
    } else {
        vec3 a = abs(v_rel);
        if (max(a.x, max(a.y, a.z)) < 0.5 + seam) discard;
    }
    if (u_shadow_caster.y > 0.5) {
        // Simplified terrain can rise above a light standing on the real
        // ground and wall it in; near the light only real (voxel and
        // structure) geometry occludes.
        if (length(v_rel) < 1.5 * u_lod_patch.w + 1.0) discard;
        vec2 world = v_rel.xz + u_shadow_light.xz;
        vec2 uv = (world - u_detail_origin.xy) / u_detail_origin.z;
        if (all(greaterThanEqual(uv, vec2_splat(0.0)))
            && all(lessThan(uv, vec2_splat(1.0)))) {
            if (texture2D(u_detail_map, uv).r > 0.5) discard;
        }
    }
    gl_FragColor = vec4(length(v_rel) * u_shadow_caster.x, 0.0, 0.0, 1.0);
}
