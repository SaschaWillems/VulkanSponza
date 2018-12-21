#version 450

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inColor;
layout (location = 3) in vec3 inNormal;
layout (location = 4) in vec3 inTangent;
layout (location = 5) in vec3 inBitangent;

layout (binding = 0) uniform UBO 
{
	mat4 projection;
	mat4 model;
	mat4 view;
} ubo;

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec2 outUV;
layout (location = 2) out vec3 outColor;
layout (location = 3) out vec3 outWorldPos;
layout (location = 4) out vec3 outTangent;
layout (location = 5) out vec3 outBitangent;

void main() 
{
	gl_Position = ubo.projection * ubo.view * ubo.model * inPos;
	
	outUV = inUV;
	outUV.t = 1.0 - outUV.t;

	// Vertex position in world space
	outWorldPos = inPos.xyz;

	outWorldPos = vec3(ubo.view * ubo.model * inPos);

	// GL to Vulkan coord space
	//outWorldPos.y = -outWorldPos.y;
	
	// Normal in world space
	mat3 mNormal = transpose(inverse(mat3(ubo.model)));
	outNormal = mNormal * normalize(inNormal);	

	// Normal in view space
	mat3 normalMatrix = transpose(inverse(mat3(ubo.view * ubo.model)));
	outNormal = normalMatrix * inNormal;

	outTangent = mNormal * normalize(inTangent);

	// Currently just vertex color
	outColor = inColor;

	outBitangent = inBitangent;
}
