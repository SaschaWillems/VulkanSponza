#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (binding = 1) uniform sampler2D samplerPosition;
layout (binding = 2) uniform sampler2D samplerNormal;
layout (binding = 3) uniform usampler2D samplerAlbedo;
layout (binding = 4) uniform sampler2D samplerSSAO;

layout (location = 0) in vec3 inUV;

layout (location = 0) out vec4 outFragColor;

void main() 
{
	vec3 components[3];
	//components[0] = texture(samplerPosition, inUV.st).rgb;  
	components[1] = texture(samplerNormal, inUV.st).rgb;  
	ivec2 texDim = textureSize(samplerAlbedo, 0);
	uvec4 albedo = texelFetch(samplerAlbedo, ivec2(inUV.st * texDim ), 0);
//	uvec4 albedo = texture(samplerAlbedo, inUV.st, 0);

	vec4 color;
	color.rg = unpackHalf2x16(albedo.r);
	color.ba = unpackHalf2x16(albedo.g);
	vec4 spec;
	spec.rg = unpackHalf2x16(albedo.b);
	vec4 ssao = texture(samplerSSAO, inUV.st);

	//components[2] = vec3(spec.r);
	components[2] = vec3(ssao.r);
	components[0] = color.rgb;

	// Select component depending on z coordinate of quad
	highp int index = int(inUV.z);
	outFragColor.rgb = components[index];
}