// Shared by voxel terrain and distant terrain. World-space texture mapping
// and the same light/fog response keep the two representations consistent.
uniform vec4 u_world_eye;

vec3 surfaceLight(vec3 normal, vec3 albedo, float exposure)
{
    vec3 sun = normalize(vec3(-0.35, 0.85, 0.4));
    float directional = 0.72 + 0.28 * max(dot(normalize(normal), sun), 0.0);
    return albedo * directional * max(0.08, exposure);
}
vec3 surfaceFog(vec3 position, vec3 color)
{
    float distanceToEye = length(position - u_world_eye.xyz);
    float fog = smoothstep(u_world_eye.w * 0.45, u_world_eye.w, distanceToEye);
    return mix(color, vec3(0.68, 0.77, 0.84), fog);
}
vec4 surfaceTexture(vec3 position, vec3 normal, float layer)
{
    vec3 weights = pow(abs(normal), vec3(8.0));
    weights /= max(dot(weights, vec3(1.0)), 0.0001);
    return texture2DArray(u_texture_array, vec3(position.zy, layer)) * weights.x
         + texture2DArray(u_texture_array, vec3(position.xz, layer)) * weights.y
         + texture2DArray(u_texture_array, vec3(position.xy, layer)) * weights.z;
}
