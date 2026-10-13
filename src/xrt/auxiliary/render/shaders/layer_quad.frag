// Copyright 2020-2023, Collabora Ltd.
//
// Author: Jakob Bornecrantz <jakob@collabora.com>
// Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
//
// SPDX-License-Identifier: BSL-1.0

#version 460

layout (binding = 0, std140) uniform Config
{
	vec4 post_transform;
	mat4 mvp;
	vec4 color_scale;
	vec4 color_bias;
} ubo;

layout (binding = 1) uniform sampler2D image;

layout (location = 0)  in vec2 uv;
layout (location = 0) out vec4 out_color;


// Mitchell-Netravali (B = C = 1/3) as a radial weight, like SteamVR's overlay filter
float mitchell(float x)
{
	float x2 = x * x;
	float x3 = x2 * x;
	if (x < 1.0) {
		return (7.0 * x3 - 12.0 * x2 + 16.0 / 3.0) / 6.0;
	}
	if (x < 2.0) {
		return (-7.0 / 3.0 * x3 + 12.0 * x2 - 20.0 * x + 32.0 / 3.0) / 6.0;
	}
	return 0.0;
}

// 5x5 texel filter whose kernel widens with the pixel's footprint (SteamVR's distort_geom_drawregion_multitap_ps)
vec4 multitap(vec2 uv)
{
	vec2 size = vec2(textureSize(image, 0));
	vec2 pos = uv * size;
	vec2 dx = dFdxFine(uv) * size;
	vec2 dy = dFdyFine(uv) * size;
	float scale = inversesqrt(max(1.0, 0.5 * (dot(dx, dx) + dot(dy, dy))));
	vec2 center = floor(pos) + 0.5;
	vec4 sum = vec4(0.0);
	float weight = 0.0;
	for (int x = -2; x <= 2; x++) {
		for (int y = -2; y <= 2; y++) {
			vec2 p = center + vec2(x, y);
			float w = mitchell(length((p - pos) * scale));
			sum += textureLod(image, p / size, 0.0) * w;
			weight += w;
		}
	}
	return sum / weight;
}

void main ()
{
	out_color = multitap(uv);
	out_color = clamp(out_color * ubo.color_scale + ubo.color_bias, 0.0, 1.0);
}
