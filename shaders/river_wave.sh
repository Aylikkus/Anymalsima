// Both meshes use the same wrapped world coordinates. Sharing the height
// field at corners keeps the surface closed where the current turns.
uniform vec4 u_world_water;

float riverRise(vec2 position)
{
    vec2 phase = mod(position, 16.0) * 0.3926990817;
    return 0.045 * sin(phase.x + 2.0 * phase.y - 2.1 * u_world_water.x)
         + 0.022 * sin(2.0 * phase.x - phase.y + 1.4 * u_world_water.x);
}
