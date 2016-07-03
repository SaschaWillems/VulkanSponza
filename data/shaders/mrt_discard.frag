#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (binding = 1) uniform sampler2D samplerColor;
layout (binding = 2) uniform sampler2D samplerSpecular;
layout (binding = 3) uniform sampler2D samplerNormal;

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inColor;
layout (location = 3) in vec3 inWorldPos;
layout (location = 4) in vec3 inTangent;

layout (location = 0) out vec4 outPosition;
layout (location = 1) out vec4 outNormal;
layout (location = 2) out uvec4 outAlbedo;

void main() 
{
	outPosition = vec4(inWorldPos, 1.0);

	vec3 N = normalize(inNormal);
	vec3 T = normalize(inTangent);
	vec3 B = cross(N, T);
	mat3 TBN = mat3(T, B, N);
	vec3 nm = texture(samplerNormal, inUV).xyz * 2.0 - vec3(1.0);
	nm = TBN * normalize(nm);
	outNormal = vec4(nm, 0.0);

	vec4 color = texture(samplerColor, inUV);

	if (color.a < 0.5)
	{
		discard;
	}

	// Pack
	float specular = texture(samplerSpecular, inUV).r;

	outAlbedo.r = packHalf2x16(color.rg);
	outAlbedo.g = packHalf2x16(color.ba);
	outAlbedo.b = packHalf2x16(vec2(specular, 0.0);
}