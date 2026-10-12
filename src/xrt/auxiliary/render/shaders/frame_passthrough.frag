#version 460
layout(binding = 0) uniform sampler2D luma;
layout(binding = 1) uniform sampler2D chroma;
layout(push_constant) uniform Camera {
    vec4 intrinsics;
    vec4 radial;
    vec4 rotation[3];
    vec4 tangent;
    vec4 map_scale;
    vec4 translation;
    vec4 mesh_rotation[3];
    vec4 mesh_translation;
    vec4 mesh_grid;      // w: debug tint for what came from the room mesh
} camera;
layout(location = 0) in vec2 sample_uv;
layout(location = 1) in vec2 flags;
layout(location = 0) out vec4 color;
void main()
{
    if (flags.x < 0.999 || any(lessThan(sample_uv, vec2(0))) || any(greaterThan(sample_uv, vec2(1)))) {
        color = vec4(0, 0, 0, 1); return;
    }
    float y = (texture(luma, sample_uv).r - 16.0 / 255.0) * 1.164383;
    vec2 c = texture(chroma, sample_uv).rg - vec2(128.0 / 255.0);
    vec3 rgb = clamp(vec3(y + 1.596027 * c.y, y - 0.391762 * c.x - 0.812968 * c.y,
                         y + 2.017232 * c.x), 0.0, 1.0);
    color = vec4(mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                     greaterThan(rgb, vec3(0.04045))), 1.0);
    if (flags.y > 0.5 && camera.mesh_grid.w > 0.0) color.rb *= 0.5;
}
