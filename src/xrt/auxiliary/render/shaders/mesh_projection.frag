// SPDX-License-Identifier: BSL-1.0
#version 450

layout(binding = 0) uniform sampler2D tex_sampler;
layout(location = 0) in vec2 in_ruv;
layout(location = 1) in vec2 in_guv;
layout(location = 2) in vec2 in_buv;
layout(location = 0) out vec4 out_color;

void main()
{
	vec4 green = texture(tex_sampler, in_guv);
	out_color = vec4(texture(tex_sampler, in_ruv).r, green.g, texture(tex_sampler, in_buv).b, green.a);
}
