// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0

// Draws a quad layer straight into the distorted target, like SteamVR's distort_geom_simplified_ps: the quad is
// sampled once per pixel (anisotropic, implicit derivatives in panel space) instead of going through the per-view
// scratch image and a second resampling.

#version 450

layout (binding = 0) uniform sampler2D image;
layout (binding = 1, std140) uniform Config
{
	// Same as mesh.vert, post_transform is the scratch rect the mesh samples.
	vec4 vertex_rot;
	vec4 post_transform;
	vec4 pre_transform;
	mat4 transform_scanout_begin;
	mat4 transform_scanout_end;

	// Scratch NDC to quad plane (inverse homography, rows).
	vec4 ndc_to_quad[3];
	vec4 quad_post_transform;
	vec4 color_scale;
	vec4 color_bias;
} ubo;

layout (location = 0) in vec2 in_ruv;
layout (location = 1) in vec2 in_guv;
layout (location = 2) in vec2 in_buv;
layout (location = 0) out vec4 out_color;

void main()
{
	vec2 ndc = (in_guv - ubo.post_transform.xy) / ubo.post_transform.zw * 2.0 - 1.0;
	vec3 h = vec3(dot(ubo.ndc_to_quad[0].xyz, vec3(ndc, 1.0)),
	              dot(ubo.ndc_to_quad[1].xyz, vec3(ndc, 1.0)),
	              dot(ubo.ndc_to_quad[2].xyz, vec3(ndc, 1.0)));
	vec2 pos = h.xy / h.z;
	vec2 quad_uv = vec2(pos.x + 0.5, 0.5 - pos.y);
	vec2 uv = fma(quad_uv, ubo.quad_post_transform.zw, ubo.quad_post_transform.xy);

	// Derivatives before the discard, so pixels outside the quad skip the texture fetch.
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);

	if (h.z <= 0.0 || any(lessThan(quad_uv, vec2(0.0))) || any(greaterThan(quad_uv, vec2(1.0)))) {
		discard;
	}

	vec4 color = textureGrad(image, uv, dx, dy);
	if (color.a <= 0.0) {
		discard; // Like SteamVR for blended overlays, so premultiplied rgb without alpha adds nothing.
	}
	out_color = clamp(color * ubo.color_scale + ubo.color_bias, 0.0, 1.0);
}
