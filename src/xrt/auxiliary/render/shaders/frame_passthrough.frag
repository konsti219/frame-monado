#version 460
layout(binding = 0) uniform sampler2D luma;
layout(binding = 1) uniform sampler2D chroma;
layout(binding = 2) uniform sampler2D correction;
layout(push_constant) uniform Camera {
    vec4 intrinsics;
    vec4 radial;
    vec4 rotation[3];
    vec4 tangent;
    vec4 map_scale;
    vec4 translation;
} camera;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main()
{
    vec3 eye_ray = vec3(camera.tangent.xy + uv * camera.tangent.zw, -1.0);
    vec3 ray = vec3(dot(camera.rotation[0].xyz, eye_ray),
                    dot(camera.rotation[1].xyz, eye_ray),
                    dot(camera.rotation[2].xyz, eye_ray));
    ray += camera.translation.xyz * (length(eye_ray) * camera.translation.w);
    if (ray.z <= 0.0) { color = vec4(0, 0, 0, 1); return; }
    float r = length(ray.xy);
    float theta = atan(r, ray.z);
    float t2 = theta * theta;
    float distorted = theta * (1.0 + t2 * (camera.radial.x + t2 * (camera.radial.y +
                       t2 * (camera.radial.z + t2 * camera.radial.w))));
    vec2 pixel = camera.intrinsics.zw + camera.intrinsics.xy * ray.xy * distorted / max(r, 1e-8);
    if (any(notEqual(camera.map_scale.xy, vec2(0)))) {
        vec2 map_uv = vec2(0.5) + (pixel - camera.intrinsics.zw) * camera.map_scale.xy;
        pixel += texture(correction, map_uv).rg * vec2(camera.rotation[0].w, camera.rotation[1].w);
    }
    vec2 sample_uv = pixel / camera.map_scale.zw;
    if (any(lessThan(sample_uv, vec2(0))) || any(greaterThan(sample_uv, vec2(1)))) {
        color = vec4(0, 0, 0, 1); return;
    }
    float y = (texture(luma, sample_uv).r - 16.0 / 255.0) * 1.164383;
    vec2 c = texture(chroma, sample_uv).rg - vec2(128.0 / 255.0);
    vec3 rgb = clamp(vec3(y + 1.596027 * c.y, y - 0.391762 * c.x - 0.812968 * c.y,
                         y + 2.017232 * c.x), 0.0, 1.0);
    color = vec4(mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                     greaterThan(rgb, vec3(0.04045))), 1.0);
}
