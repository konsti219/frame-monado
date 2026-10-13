// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0

// Draws a quad or cylinder layer straight into the distorted target, like SteamVR's distort_geom_simplified_ps: the
// layer is sampled once per pixel (anisotropic, implicit derivatives in panel space) instead of going through the
// per-view scratch image and a second resampling.

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

	// Cylinders: scratch NDC to ray direction in cylinder space (rows), ray origin and radius, then central angle,
	// height and 1 for a cylinder.
	vec4 ndc_to_dir[3];
	vec4 cylinder_origin;
	vec4 cylinder_params;
} ubo;

layout (location = 0) in vec2 in_ruv;
layout (location = 1) in vec2 in_guv;
layout (location = 2) in vec2 in_buv;
layout (location = 0) out vec4 out_color;

void main()
{
	vec2 ndc = (in_guv - ubo.post_transform.xy) / ubo.post_transform.zw * 2.0 - 1.0;
	vec2 quad_uv;
	bool hit;
	if (ubo.cylinder_params.z == 0.0) {
		vec3 h = vec3(dot(ubo.ndc_to_quad[0].xyz, vec3(ndc, 1.0)),
		              dot(ubo.ndc_to_quad[1].xyz, vec3(ndc, 1.0)),
		              dot(ubo.ndc_to_quad[2].xyz, vec3(ndc, 1.0)));
		vec2 pos = h.xy / h.z;
		quad_uv = vec2(pos.x + 0.5, 0.5 - pos.y);
		hit = h.z > 0.0;
	} else {
		// Ray against the cylinder x^2 + z^2 = r^2 (axis y): the nearer hit inside the layer, else the farther one.
		vec3 o = ubo.cylinder_origin.xyz;
		float r = ubo.cylinder_origin.w;
		vec3 dir = vec3(dot(ubo.ndc_to_dir[0].xyz, vec3(ndc, 1.0)),
		                dot(ubo.ndc_to_dir[1].xyz, vec3(ndc, 1.0)),
		                dot(ubo.ndc_to_dir[2].xyz, vec3(ndc, 1.0)));
		float a = dot(dir.xz, dir.xz);
		float b = dot(o.xz, dir.xz);
		float c = dot(o.xz, o.xz) - r * r;
		float disc = b * b - a * c;
		float s = sqrt(max(disc, 0.0));
		vec2 t = vec2(-b - s, -b + s) / a;
		vec3 p0 = o + t.x * dir;
		vec3 p1 = o + t.y * dir;
		vec2 uv0 = vec2(atan(p0.x, -p0.z) / ubo.cylinder_params.x + 0.5, 0.5 - p0.y / ubo.cylinder_params.y);
		vec2 uv1 = vec2(atan(p1.x, -p1.z) / ubo.cylinder_params.x + 0.5, 0.5 - p1.y / ubo.cylinder_params.y);
		bool in0 = t.x > 0.0 && all(greaterThanEqual(uv0, vec2(0.0))) && all(lessThanEqual(uv0, vec2(1.0)));
		quad_uv = in0 ? uv0 : uv1;
		hit = disc > 0.0 && a > 0.0 && (in0 || t.y > 0.0);
	}
	vec2 uv = fma(quad_uv, ubo.quad_post_transform.zw, ubo.quad_post_transform.xy);

	// Derivatives before the discard, so pixels outside the layer skip the texture fetch.
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);

	if (!hit || any(lessThan(quad_uv, vec2(0.0))) || any(greaterThan(quad_uv, vec2(1.0)))) {
		discard;
	}

	vec4 color = textureGrad(image, uv, dx, dy);
	if (color.a <= 0.0) {
		discard; // Like SteamVR for blended overlays, so premultiplied rgb without alpha adds nothing.
	}
	out_color = clamp(color * ubo.color_scale + ubo.color_bias, 0.0, 1.0);
}
