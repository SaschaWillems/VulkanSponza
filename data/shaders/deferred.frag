#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (binding = 1) uniform sampler2D samplerposition;
layout (binding = 2) uniform sampler2D samplerNormal;
layout (binding = 3) uniform sampler2D samplerAlbedo;

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outFragcolor;

struct Light {
	vec4 position;
	vec4 color;
	float radius;
	float quadraticFalloff;
	float linearFalloff;
	float _pad;
};

#define NUM_LIGHTS 13

layout (binding = 4) uniform UBO 
{
	Light lights[NUM_LIGHTS];
	vec4 viewPos;
} ubo;


void main() 
{
	// Get G-Buffer values
	vec3 fragPos = texture(samplerposition, inUV).rgb;
	vec3 normal = texture(samplerNormal, inUV).rgb;
	vec4 albedo = texture(samplerAlbedo, inUV);
	
	// todo: from specular map
	#define specularStrength 1.0

	vec3 ambient = vec3(0.0);	
	vec3 fragcolor  = ambient;
	
	for(int i = 0; i < NUM_LIGHTS; ++i)
	{
		// Light to fragment
		vec3 L = ubo.lights[i].position.xyz - fragPos;
		float dist = length(L);
		L = normalize(L);

		// Viewer to fragment
		vec3 V = ubo.viewPos.xyz - fragPos;
		V = normalize(V);

		// Attenuation
		float atten = ubo.lights[i].radius / (pow(dist, 2.0) + 1.0);

		// Diffuse part
		vec3 N = normalize(normal);
		float NdotL = max(0.0, dot(N, L));
		vec3 diff = ubo.lights[i].color.rgb * albedo.rgb * NdotL * atten;

		// Specular part
		vec3 R = reflect(-L, N);
		float NdotR = max(0.0, dot(R, V));
		vec3 spec = ubo.lights[i].color.rgb * specularStrength * pow(NdotR, 16.0) * atten;

		fragcolor += diff + spec;				
	}    	
   
	outFragcolor = vec4(fragcolor, 1.0);	
}