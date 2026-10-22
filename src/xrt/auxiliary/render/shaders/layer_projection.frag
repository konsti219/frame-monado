// Copyright 2020-2023, Collabora Ltd.
//
// Author: Jakob Bornecrantz <jakob@collabora.com>
// Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
//
// SPDX-License-Identifier: BSL-1.0

#version 460

#define LUT_SIZE 512

layout (binding = 0, std140) uniform Config
{
	vec4 post_transform;
	vec4 to_tangent;
	mat4 mvp;
	vec4 color_scale;
	vec4 color_bias;
	uvec4 foveation_enabled;
	vec4 lut_x[LUT_SIZE];
	vec4 lut_y[LUT_SIZE];
} ubo;

layout (binding = 1) uniform sampler2D image;

layout (location = 0)  in vec2 uv;
layout (location = 0) out vec4 out_color;


void main ()
{
	vec2 image_uv = uv;
	if (ubo.foveation_enabled.x != 0u) {
		vec2 p = clamp((uv - ubo.post_transform.xy) / ubo.post_transform.zw, 0.0, 1.0) * float(LUT_SIZE);
		ivec2 i = min(ivec2(p), ivec2(LUT_SIZE - 1));
		vec2 f = p - vec2(i);
		vec2 ax = ubo.lut_x[i.x].xy;
		vec2 ay = ubo.lut_y[i.y].xy;
		vec2 expanded = vec2(mix(ax.x, ax.y, f.x), mix(ay.x, ay.y, f.y));
		image_uv = ubo.post_transform.xy + expanded * ubo.post_transform.zw;
	}
	out_color = texture(image, image_uv);
	out_color = clamp(out_color * ubo.color_scale + ubo.color_bias, 0.0, 1.0);
}
