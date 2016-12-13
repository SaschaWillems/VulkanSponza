#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (binding = 1) uniform sampler2D samplerSmoke;
layout (binding = 2) uniform sampler2DArray samplerFire;
layout (binding = 3) uniform sampler2D samplerPositionDepth;

layout (location = 0) in vec4 inColor;
layout (location = 1) in float inPointSize;
layout (location = 2) in float inAlpha;
layout (location = 3) in flat int inType;
layout (location = 4) in float inRotation;
layout (location = 5) in vec2 inViewportDim;
layout (location = 6) in float inArrayPos;

layout (location = 0) out vec4 outColor;

layout (constant_id = 0) const float NEAR_PLANE = 1.0f;
layout (constant_id = 1) const float FAR_PLANE = 512.0f;

float linearDepth(float depth)
{
	float z = depth * 2.0f - 1.0f; 
	return (2.0f * NEAR_PLANE * FAR_PLANE) / (FAR_PLANE + NEAR_PLANE - z * (FAR_PLANE - NEAR_PLANE));	
}

void main () 
{
	// Sample depth from deferred depth buffer and discard if obscured
	vec2 ndcPos = gl_FragCoord.xy / inViewportDim.xy; 
	float depth = texture(samplerPositionDepth, ndcPos).w;
	if (linearDepth(gl_FragCoord.z) > depth)
	{
		discard;
	};

	vec4 color;
	float alpha = (inAlpha <= 1.0) ? inAlpha : 2.0 - inAlpha;
	
	// Rotate texture coordinates
	// Rotate UV	
	float rotCenter = 0.5;
	float rotCos = cos(inRotation);
	float rotSin = sin(inRotation);
	vec2 rotUV = vec2(
		rotCos * (gl_PointCoord.x - rotCenter) + rotSin * (gl_PointCoord.y - rotCenter) + rotCenter,
		rotCos * (gl_PointCoord.y - rotCenter) - rotSin * (gl_PointCoord.x - rotCenter) + rotCenter);

	
	if (inType == 0) 
	{
		// Flame
		color = texture(samplerFire, vec3(rotUV, inArrayPos));
		outColor.a = 0.0;
	}
	else
	{
		// Smoke
		color = texture(samplerSmoke, rotUV);
		outColor.a = color.a * alpha;
	}

	outColor.rgb = color.rgb * inColor.rgb * alpha;
}
