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

// Far lights (LightRenderer): lights reaching beyond the voxel near field,
// evaluated per pixel. Four vec4 per light:
//   [0] xyz = world position (copy nearest the eye), w = radius
//   [1] rgb = color * intensity, w = falloff exponent
//   [2] xyz = world forward, w = cos(outer angle), or -2 without a cone
//   [3] x = cos(inner angle), y = 1 when the voxel near field covers the
//       first SURFACE_NEAR_RANGE blocks (the term fades in over that band)
#define SURFACE_MAX_FAR_LIGHTS 32
#define SURFACE_NEAR_RANGE 15.0
uniform vec4 u_lights[128];
// x = far light count, y = sky darkening (0 = full daylight, 1 = no sky
// light). Zero when unbound, so programs without lights stay sky-lit.
uniform vec4 u_light_info;

vec3 surfaceFarLight(vec3 position, vec3 normal)
{
    vec3 sum = vec3_splat(0.0);
    int count = int(u_light_info.x + 0.5);
    for (int i = 0; i < SURFACE_MAX_FAR_LIGHTS; ++i) {
        if (i >= count) break;
        vec4 placement = u_lights[i * 4];
        vec4 color = u_lights[i * 4 + 1];
        vec4 cone = u_lights[i * 4 + 2];
        vec4 extra = u_lights[i * 4 + 3];
        vec3 toLight = placement.xyz - position;
        float d = length(toLight);
        if (d >= placement.w) continue;
        vec3 l = toLight / max(d, 0.0001);
        float strength = pow(max(1.0 - d / placement.w, 0.0), color.w);
        if (cone.w > -1.5)
            strength *= smoothstep(cone.w, extra.x, dot(-l, cone.xyz));
        if (extra.y > 0.5)
            strength *= smoothstep(SURFACE_NEAR_RANGE - 4.0,
                                   SURFACE_NEAR_RANGE, d);
        float facing = SURFACE_AMBIENT
                     + SURFACE_DIFFUSE * max(dot(normal, l), 0.0);
        sum += color.rgb * (strength * facing);
    }
    return sum;
}

// Block light chroma (LightVolume BA channels, BlockLightPropagator::
// encodeChroma) to an RGB tint whose brightest channel is 1.
vec3 surfaceLightTint(vec2 chroma)
{
    float co = (chroma.x * 255.0 - 128.0) / 63.5;
    float cg = (chroma.y * 255.0 - 128.0) / 127.0;
    vec3 rgb = max(vec3(1.0 + co - cg, 1.0 + cg, 1.0 - co - cg),
                   vec3_splat(0.0));
    return rgb / max(max(rgb.r, rgb.g), max(rgb.b, 0.0001));
}

// light.x = sky light, light.y = block (artificial) light, both 0..1;
// blockTint colors the block light. Sky light comes from the sun direction.
// Block light comes from blockDir (toward brighter space); blockDirStrength
// fades that to non-directional where the light field is flat. far is
// surfaceFarLight(), already scaled by how much sky the point sees.
vec3 surfaceLight(vec3 normal, vec3 albedo, vec2 light, vec3 blockTint,
                  vec3 blockDir, float blockDirStrength, vec3 far)
{
    float sun = max(dot(normal, surfaceSunDirection()), 0.0);
    float daylight = 1.0 - clamp(u_light_info.y, 0.0, 1.0);
    float sky = light.x * daylight * (SURFACE_AMBIENT + SURFACE_DIFFUSE * sun);
    float toward = mix(1.0, max(dot(normal, blockDir), 0.0), blockDirStrength);
    vec3 block = blockTint * (light.y
                              * (SURFACE_AMBIENT + SURFACE_DIFFUSE * toward))
               + far;
    vec3 lit = max(vec3_splat(sky), block);
    return albedo * max(lit, vec3_splat(SURFACE_MIN_LIGHT));
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
