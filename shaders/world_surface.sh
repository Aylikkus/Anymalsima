// Shared by voxel terrain and distant terrain. World-space texture mapping
// and the same light/fog response keep the two representations consistent.
//
// Including programs declare u_texture_array and u_normal_array (the normal
// array uses the same layer index as the albedo array).
uniform vec4 u_world_eye;

// Light response: ambient + diffuse * N.L, both scaled by the light level.
#define SURFACE_AMBIENT 0.6
#define SURFACE_DIFFUSE 0.4
#define SURFACE_MIN_LIGHT 0.08
// Texture rows are uploaded top-down, so +t points down the image. Normal
// maps authored OpenGL-style (+green = up) need -1 here, DirectX-style +1.
#define NORMAL_GREEN_SIGN -1.0

vec3 surfaceSunDirection()
{
    return normalize(vec3(-0.35, 0.85, 0.4));
}

vec3 triplanarWeights(vec3 normal)
{
    vec3 weights = pow(abs(normal), vec3(8.0, 8.0, 8.0));
    return weights / max(dot(weights, vec3(1.0, 1.0, 1.0)), 0.0001);
}

vec3 decodeTangentNormal(vec4 texel)
{
    vec3 n = texel.xyz * 2.0 - 1.0;
    n.y *= NORMAL_GREEN_SIGN;
    // Box-filtered mips shorten the vectors; renormalize.
    return n * inversesqrt(max(dot(n, n), 0.000001));
}

// Normal-mapped world normal for the triplanar projection in
// surfaceTexture(). Each projection plane's TBN comes from its texture axes:
//   X plane: uv = pos.zy  -> T = +Z, B = +Y
//   Y plane: uv = pos.xz  -> T = +X, B = +Z
//   Z plane: uv = pos.xy  -> T = +X, B = +Y
// N is the geometric normal, so mirrored (negative) faces stay correct and
// smooth LOD normals blend like a UDN triplanar.
vec3 surfaceNormal(vec3 position, vec3 normal, float layer)
{
    vec3 weights = triplanarWeights(normal);
    vec3 nx = decodeTangentNormal(
        texture2DArray(u_normal_array, vec3(position.zy, layer)));
    vec3 ny = decodeTangentNormal(
        texture2DArray(u_normal_array, vec3(position.xz, layer)));
    vec3 nz = decodeTangentNormal(
        texture2DArray(u_normal_array, vec3(position.xy, layer)));
    vec3 wx = vec3(0.0, nx.y, nx.x) + normal * nx.z;
    vec3 wy = vec3(ny.x, 0.0, ny.y) + normal * ny.z;
    vec3 wz = vec3(nz.x, nz.y, 0.0) + normal * nz.z;
    return normalize(wx * weights.x + wy * weights.y + wz * weights.z);
}

// light.x = sky light, light.y = block (artificial) light, both 0..1.
// Sky light comes from the sun direction. Block light comes from blockDir
// (toward brighter space); blockDirStrength fades that to non-directional
// where the light field is flat.
vec3 surfaceLight(vec3 normal, vec3 albedo, vec2 light, vec3 blockDir,
                  float blockDirStrength)
{
    float sun = max(dot(normal, surfaceSunDirection()), 0.0);
    float sky = light.x * (SURFACE_AMBIENT + SURFACE_DIFFUSE * sun);
    float toward = mix(1.0, max(dot(normal, blockDir), 0.0), blockDirStrength);
    float block = light.y * (SURFACE_AMBIENT + SURFACE_DIFFUSE * toward);
    return albedo * max(max(sky, block), SURFACE_MIN_LIGHT);
}
vec3 surfaceFog(vec3 position, vec3 color)
{
    float distanceToEye = length(position - u_world_eye.xyz);
    float fog = smoothstep(u_world_eye.w * 0.45, u_world_eye.w, distanceToEye);
    return mix(color, vec3(0.68, 0.77, 0.84), fog);
}
vec4 surfaceTexture(vec3 position, vec3 normal, float layer)
{
    vec3 weights = triplanarWeights(normal);
    return texture2DArray(u_texture_array, vec3(position.zy, layer)) * weights.x
         + texture2DArray(u_texture_array, vec3(position.xz, layer)) * weights.y
         + texture2DArray(u_texture_array, vec3(position.xy, layer)) * weights.z;
}
