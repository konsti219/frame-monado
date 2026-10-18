// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0

// An opaque projection layer with a foveated image (XR_FRAME_foveated_projection), drawn straight into the distorted
// target: the view position from the mesh goes through per-axis lookup tables into the image.

#version 450

#define LUT_SIZE 512

layout (binding = 0) uniform sampler2D tex_sampler;
layout (binding = 1, std140) uniform Config
{
	// Same as mesh.vert, post_transform is the image rect the mesh samples.
	vec4 vertex_rot;
	vec4 post_transform;
	vec4 pre_transform;
	mat4 transform_scanout_begin;
	mat4 transform_scanout_end;

	// Per interval i: image positions (0..1) at view positions i / LUT_SIZE and (i + 1) / LUT_SIZE.
	vec4 lut_x[LUT_SIZE];
	vec4 lut_y[LUT_SIZE];
} ubo;

layout (location = 0) in vec2 in_ruv;
layout (location = 1) in vec2 in_guv;
layout (location = 2) in vec2 in_buv;
layout (location = 0) out vec4 out_color;

vec2 image_uv(vec2 uv)
{
	vec2 p = clamp((uv - ubo.post_transform.xy) / ubo.post_transform.zw, 0.0, 1.0) * float(LUT_SIZE);
	ivec2 i = min(ivec2(p), ivec2(LUT_SIZE - 1));
	vec2 f = p - vec2(i);
	vec2 ax = ubo.lut_x[i.x].xy;
	vec2 ay = ubo.lut_y[i.y].xy;
	float x = mix(ax.x, ax.y, f.x);
	float y = mix(ay.x, ay.y, f.y);
	return ubo.post_transform.xy + vec2(x, y) * ubo.post_transform.zw;
}

void main()
{
	// Outside the view is black, as the plain mesh shader's border sampler gives
	vec2 view = (in_guv - ubo.post_transform.xy) / ubo.post_transform.zw;
	if (any(lessThan(view, vec2(0.0))) || any(greaterThan(view, vec2(1.0)))) {
		out_color = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	vec3 g = texture(tex_sampler, image_uv(in_guv)).rgb;
	// The display corrects colour fringes, so all channels usually share the green coordinates
	if (in_ruv == in_guv && in_buv == in_guv) {
		out_color = vec4(g, 1.0);
	} else {
		out_color = vec4(texture(tex_sampler, image_uv(in_ruv)).r, g.g, texture(tex_sampler, image_uv(in_buv)).b, 1.0);
	}
}
