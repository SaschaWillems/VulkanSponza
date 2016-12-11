#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (binding = 1) uniform sampler2D samplerSky;

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outPosition;
layout (location = 1) out vec4 outNormal;
layout (location = 2) out uvec4 outAlbedo;

void main() 
{
	vec4 color = texture(samplerSky, inUV);

	outAlbedo.r = packHalf2x16(color.rg);
	outAlbedo.g = packHalf2x16(color.ba);
	outAlbedo.b = 0;

	outNormal = vec4(0.0);
	outPosition = vec4(0.0);
}