#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec4 inColor;
layout (location = 2) in float inAlpha;
layout (location = 3) in float inSize;
layout (location = 4) in float inRotation;
layout (location = 5) in int inType;

layout (location = 0) out vec4 outColor;
layout (location = 1) out float outPointSize;
layout (location = 2) out float outAlpha;
layout (location = 3) out flat int outType;
layout (location = 4) out float outRotation;
layout (location = 5) out vec2 outViewportDim;

layout (binding = 0) uniform UBO 
{
	mat4 projection;
	mat4 view;
	mat4 model;
	vec2 viewportDim;
} ubo;

void main () 
{
	gl_PointSize = 8.0;
	outColor = inColor;
	outAlpha = inAlpha;
	outType = inType;
	outRotation = inRotation;
	outViewportDim = ubo.viewportDim;
	  
	vec4 eyePos = ubo.view * ubo.model * vec4(inPos.xyz, 1.0);
	vec4 projVoxel = ubo.projection * vec4(gl_PointSize, gl_PointSize, eyePos.z, eyePos.w);
	vec2 projSize = ubo.viewportDim * projVoxel.xy / projVoxel.w;

	outPointSize = inSize * 0.25 * (projSize.x + projSize.y);
	
	gl_PointSize = outPointSize;
		
	gl_Position = ubo.projection * ubo.view * ubo.model * vec4(inPos.xyz, 1.0);	
	
	float pointDist = (gl_Position.w == 0.0) ? 0.00001 : gl_Position.w;
	float viewportAR = ubo.viewportDim.x / ubo.viewportDim.y;
	
	gl_PointSize = (((inSize * 1024.0 * viewportAR) / pointDist) * viewportAR);	
	
}