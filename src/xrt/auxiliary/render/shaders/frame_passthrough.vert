#version 460
// Passthrough as a grid per eye: the reprojection (eye ray, room mesh hit, fisheye projection) runs per vertex and
// the fragment shader only samples the camera image, like SteamVR's tracked camera reprojection
layout(binding = 2) uniform sampler2D correction;
// RoomView3D depth map: z of each grid vertex in the mesh's pinhole frame
layout(binding = 3) uniform sampler2D mesh_depth;
layout(push_constant) uniform Camera {
    vec4 intrinsics;
    vec4 radial;
    vec4 rotation[3];    // eye to camera; w of rows 0/1: correction map scale
    vec4 tangent;
    vec4 map_scale;
    vec4 translation;    // eye origin in the camera frame, w: 1 / fallback depth (0: at infinity)
    vec4 mesh_rotation[3]; // camera to mesh frame; w: focal length, centre x, centre y in grid cells
    vec4 mesh_translation; // camera origin in the mesh frame, w: 1 when a mesh is available
    vec4 mesh_grid;      // grid width, height, iterations, debug tint
} camera;
layout(location = 0) out vec2 sample_uv;
layout(location = 1) out vec2 flags; // x: in front of the camera, y: from the mesh

const int N = 64;

// Distance along the camera-space ray from origin where it meets the depth map, or -1
float mesh_hit(vec3 origin, vec3 dir, float t)
{
    vec3 o = vec3(dot(camera.mesh_rotation[0].xyz, origin), dot(camera.mesh_rotation[1].xyz, origin),
                  dot(camera.mesh_rotation[2].xyz, origin)) + camera.mesh_translation.xyz;
    vec3 d = vec3(dot(camera.mesh_rotation[0].xyz, dir), dot(camera.mesh_rotation[1].xyz, dir),
                  dot(camera.mesh_rotation[2].xyz, dir));
    if (abs(d.z) < 1e-4) return -1.0;
    vec2 centre = vec2(camera.mesh_rotation[1].w, camera.mesh_rotation[2].w);
    for (int i = 0; i < int(camera.mesh_grid.z); i++) {
        vec3 p = o + t * d;
        if (p.z < 0.05) return -1.0;
        vec2 cell = camera.mesh_rotation[0].w * p.xy / p.z + centre;
        if (any(lessThan(cell, vec2(0.0))) || any(greaterThan(cell, camera.mesh_grid.xy - 1.0))) return -1.0;
        float z = textureLod(mesh_depth, (cell + 0.5) / camera.mesh_grid.xy, 0.0).r;
        if (z < 0.05) return -1.0;
        t = (z - o.z) / d.z;
        if (t <= 0.0) return -1.0;
    }
    return t;
}

void main()
{
    const ivec2 corners[6] = ivec2[](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1));
    int cell = gl_VertexIndex / 6;
    vec2 uv = vec2(ivec2(cell % N, cell / N) + corners[gl_VertexIndex % 6]) / float(N);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);

    vec3 eye_ray = vec3(camera.tangent.xy + uv * camera.tangent.zw, -1.0);
    vec3 dir = vec3(dot(camera.rotation[0].xyz, eye_ray),
                    dot(camera.rotation[1].xyz, eye_ray),
                    dot(camera.rotation[2].xyz, eye_ray));
    vec3 ray = dir;
    float on_mesh = 0.0;
    if (camera.translation.w > 0.0) {
        // fallback: the scene at a fixed distance from the eye
        float t = 1.0 / (camera.translation.w * length(eye_ray));
        if (camera.mesh_translation.w > 0.0) {
            float hit = mesh_hit(camera.translation.xyz, dir, t);
            if (hit > 0.0) { t = hit; on_mesh = 1.0; }
        }
        ray = camera.translation.xyz + t * dir;
    }
    flags = vec2(ray.z > 0.0 ? 1.0 : 0.0, on_mesh);
    float r = length(ray.xy);
    float theta = atan(r, max(ray.z, 1e-6));
    float t2 = theta * theta;
    float distorted = theta * (1.0 + t2 * (camera.radial.x + t2 * (camera.radial.y +
                       t2 * (camera.radial.z + t2 * camera.radial.w))));
    vec2 pixel = camera.intrinsics.zw + camera.intrinsics.xy * ray.xy * distorted / max(r, 1e-8);
    if (any(notEqual(camera.map_scale.xy, vec2(0)))) {
        vec2 map_uv = vec2(0.5) + (pixel - camera.intrinsics.zw) * camera.map_scale.xy;
        pixel += textureLod(correction, map_uv, 0.0).rg * vec2(camera.rotation[0].w, camera.rotation[1].w);
    }
    sample_uv = pixel / camera.map_scale.zw;
}
