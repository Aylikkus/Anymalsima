// Cube shadow maps of far lights (LightShadows), packed into one R32F atlas.
// Each shadowed light owns six tiles (one per cube face) holding the
// distance from the light to the nearest caster, divided by the light's
// radius (1 = nothing within the radius).
//
// Including programs declare SAMPLER2D(u_shadow_atlas, stage) first.
// Keep the face bases, tile layout and texel convention in sync with
// world/light_shadow_math.hpp.

// x = tile size in texels, y = 1 / atlas width, z = 1 / atlas height,
// w = 1 when texture rows start at the bottom (OpenGL).
uniform vec4 u_shadow_info;

#define SHADOW_COLUMNS 8.0
// LightShadowMath::GuardTexels: each face is rendered this many texels wider
// on every side than its 90 degree view.
#define SHADOW_GUARD_TEXELS 2.0

// Face of the direction v (light to point) and its normalized device
// coordinates on that face. Faces: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z;
// ndc = (dot(v, right), dot(v, up)) / dot(v, forward).
void shadowFace(vec3 v, out float face, out vec2 ndc)
{
    vec3 a = abs(v);
    if (a.x >= a.y && a.x >= a.z) {
        if (v.x > 0.0) { face = 0.0; ndc = vec2(-v.z, v.y) / a.x; }
        else           { face = 1.0; ndc = vec2( v.z, v.y) / a.x; }
    } else if (a.y >= a.z) {
        if (v.y > 0.0) { face = 2.0; ndc = vec2(v.x, -v.z) / a.y; }
        else           { face = 3.0; ndc = vec2(v.x,  v.z) / a.y; }
    } else {
        if (v.z > 0.0) { face = 4.0; ndc = vec2( v.x, v.y) / a.z; }
        else           { face = 5.0; ndc = vec2(-v.x, v.y) / a.z; }
    }
}

// Texel position (top-left origin, in texels) of ndc on a tile, and the
// tile's texel bounds.
vec2 shadowTexel(float slot, float face, vec2 ndc, out vec4 bounds)
{
    float tile = slot * 6.0 + face;
    float row = floor(tile / SHADOW_COLUMNS);
    float col = tile - row * SHADOW_COLUMNS;
    float size = u_shadow_info.x;
    bounds = vec4(col * size, row * size, (col + 1.0) * size,
                  (row + 1.0) * size);
    float scale = 1.0 + 2.0 * SHADOW_GUARD_TEXELS / size;
    vec2 uv = vec2(0.5 + 0.5 * ndc.x / scale, 0.5 - 0.5 * ndc.y / scale);
    return bounds.xy + uv * size;
}

float shadowFetch(vec2 texel, vec4 bounds)
{
    texel = clamp(texel, bounds.xy + 0.5, bounds.zw - 0.5);
    vec2 uv = texel * u_shadow_info.yz;
    if (u_shadow_info.w > 0.5) uv.y = 1.0 - uv.y;
    return texture2DLod(u_shadow_atlas, uv, 0.0).r;
}

// Distance (world tiles) from the light to the nearest caster along v.
float shadowCasterDistance(float slot, vec3 v, float radius)
{
    float face;
    vec2 ndc;
    shadowFace(v, face, ndc);
    vec4 bounds;
    vec2 texel = shadowTexel(slot, face, ndc, bounds);
    return shadowFetch(texel, bounds) * radius;
}

// Casters this close to the light (world tiles) switch the filter to the
// conservative test below.
#define SHADOW_HUG_DISTANCE 3.0

// 0 = shadowed, 1 = lit, from the 2x2 nearest texels. fromLight: point minus
// light position; normal: the receiver's geometric normal.
//
// Usually filtered bilinearly. Where a caster hugs the light (walls built
// right around a lamp) the point is lit only if every texel of the
// surrounding 4x4 (sampled at its corners and centre) sees it:
// there the lamp's faces and the walls' (both grown by the mesher's seam
// overlap) share strips the caster pass must skip, which leaves one-texel
// cracks at exactly 45 degrees that filtering would turn into thin lines of
// light.
float shadowVisibility(float slot, vec3 fromLight, vec3 normal, float radius)
{
    float d = length(fromLight);
    // A 90 degree face spans 2d world tiles at distance d.
    float texelWorld = 2.0 * d / u_shadow_info.x;
    // Surfaces seen at a grazing angle change depth quickly across a texel:
    // scale the bias with the slope so they do not shadow themselves.
    float cosAngle = clamp(dot(normal, -fromLight / max(d, 0.0001)), 0.05, 1.0);
    float slope = min(sqrt(1.0 - cosAngle * cosAngle) / cosAngle, 8.0);
    vec3 p = fromLight + normal * (1.5 * texelWorld + 0.02);
    float face;
    vec2 ndc;
    shadowFace(p, face, ndc);
    vec4 bounds;
    vec2 texel = shadowTexel(slot, face, ndc, bounds) - 0.5;
    vec2 base = floor(texel);
    vec2 f = texel - base;
    float reference =
        (length(p) - texelWorld * (1.0 + slope) - 0.05) / radius;
    float d00 = shadowFetch(base + vec2(0.5, 0.5), bounds);
    float d10 = shadowFetch(base + vec2(1.5, 0.5), bounds);
    float d01 = shadowFetch(base + vec2(0.5, 1.5), bounds);
    float d11 = shadowFetch(base + vec2(1.5, 1.5), bounds);
    float s00 = step(reference, d00);
    float s10 = step(reference, d10);
    float s01 = step(reference, d01);
    float s11 = step(reference, d11);
    // The corners of the surrounding 4x4 texels: a crack up to two texels
    // wide always has a wall texel among them.
    float e00 = shadowFetch(base + vec2(-0.5, -0.5), bounds);
    float e10 = shadowFetch(base + vec2(2.5, -0.5), bounds);
    float e01 = shadowFetch(base + vec2(-0.5, 2.5), bounds);
    float e11 = shadowFetch(base + vec2(2.5, 2.5), bounds);
    float nearest = min(min(min(d00, d10), min(d01, d11)),
                        min(min(e00, e10), min(e01, e11))) * radius;
    if (nearest < SHADOW_HUG_DISTANCE)
        return step(reference, nearest / radius);
    return mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
}
