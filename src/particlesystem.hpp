/*
* Vulkan playground for rendering Crytek's Sponza model (deferred renderer)
*
* CPU-based particle system
*
* Copyright (C) 2016 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#pragma once

#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <vulkan/vulkan.h>
#include "vulkandevice.hpp"
#include "vulkanbuffer.hpp"
#include "vulkantools.h"

#define PARTICLE_TYPE_FLAME 0
#define PARTICLE_TYPE_SMOKE 1
#define FLAME_RADIUS 2.0f

class ParticleSystem
{
private:
	vk::VulkanDevice *device;
public:
	struct Particle
	{
		glm::vec4 pos;
		glm::vec4 color;
		float alpha;
		float size;
		float rotation;
		uint32_t type;
		// Attributes not used in shader
		glm::vec4 vel;
		float rotationSpeed;
	};

	struct UniformData
	{
		glm::mat4 projection;
		glm::mat4 model;
		glm::mat4 view;
		glm::vec2 viewportDim;
	} uniformData;

	std::vector<Particle> particles;
	uint32_t particleCount;

	glm::vec3 position;
	glm::vec3 minVel;
	glm::vec3 maxVel;

	vk::Buffer buffer;
	vk::Buffer uniformBuffer;

	ParticleSystem(vk::VulkanDevice *vkdevice, uint32_t particlecount, glm::vec3 pos, glm::vec3 minvel, glm::vec3 maxvel) :
		device(vkdevice),
		particleCount(particlecount),
		position(pos),
		minVel(minvel),
		maxVel(maxvel)
	{
		// Create buffer
		particles.resize(particlecount);

		for (auto& particle : particles)
		{
			initParticle(&particle, position);
		}

		size_t bufferSize = particles.size() * sizeof(Particle);

		device->createBuffer(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
			&buffer,
			bufferSize,
			particles.data());

		buffer.map();
	};

	~ParticleSystem()
	{
		buffer.destroy();
	}

	inline float rnd(float range)
	{
		return range * (rand() / double(RAND_MAX));
	}

	void initParticle(Particle *particle, glm::vec3 emitterPos)
	{
		particle->vel = glm::vec4(0.0f, minVel.y + rnd(maxVel.y - minVel.y), 0.0f, 0.0f);
		particle->alpha = rnd(0.75f);
		particle->size = 1.0f + rnd(0.5f);
		particle->size *= 0.5f;
		particle->color = glm::vec4(1.0f);
		particle->type = PARTICLE_TYPE_FLAME;
		particle->rotation = rnd(2.0f * M_PI);
		particle->rotationSpeed = rnd(2.0f) - rnd(2.0f);

		// Get random sphere point
		float theta = rnd(2 * M_PI);
		float phi = rnd(M_PI) - M_PI / 2;
		float r = rnd(FLAME_RADIUS);

		particle->pos.x = r * cos(theta) * cos(phi);
		particle->pos.y = r * sin(phi);
		particle->pos.z = r * sin(theta) * cos(phi);

		particle->pos += glm::vec4(emitterPos, 0.0f);
	}

	void transitionParticle(Particle *particle)
	{
		switch (particle->type)
		{
		case PARTICLE_TYPE_FLAME:
			// Flame particles have a chance of turning into smoke
			if (rnd(1.0f) < 0.015f)
			{
				particle->alpha = 0.0f;
				particle->color = glm::vec4(0.15f + rnd(0.25f));
				particle->pos.x = position.x + (particle->pos.x - position.x) * 0.5f;
				particle->pos.z = position.z + (particle->pos.z - position.z) * 0.5f;
				particle->vel = glm::vec4(rnd(1.0f) - rnd(1.0f), (minVel.y * 2) + rnd(maxVel.y - minVel.y), rnd(1.0f) - rnd(1.0f), 0.0f);
				particle->size = 1.0f + rnd(0.5f);
				particle->rotationSpeed = rnd(1.0f) - rnd(1.0f);
				particle->type = PARTICLE_TYPE_SMOKE;
			}
			else
			{
				initParticle(particle, position);
			}
			break;
		case PARTICLE_TYPE_SMOKE:
			// Respawn at end of life
			initParticle(particle, position);
			break;
		}
	}

	void updateParticles(float deltaT)
	{
		float particleTimer = deltaT * 0.45f;
		for (auto& particle : particles)
		{
			switch (particle.type)
			{
			case PARTICLE_TYPE_FLAME:
				particle.pos.y -= particle.vel.y * particleTimer * 3.5f;
				particle.alpha += particleTimer * 2.5f;
				particle.size -= particleTimer * 0.5f;
				break;
			case PARTICLE_TYPE_SMOKE:
				particle.pos -= particle.vel * deltaT * 1.0f;
				particle.alpha += particleTimer * 1.25f;
				particle.size += particleTimer * 0.125f;
				particle.color -= particleTimer * 0.05f;
				break;
			}
			particle.rotation += particleTimer * particle.rotationSpeed;
			// Transition particle state
			if (particle.alpha > 2.0f)
			{
				transitionParticle(&particle);
			}
		}
		size_t size = particles.size() * sizeof(Particle);
		buffer.copyTo(particles.data(), size);
		// todo: Better synchronization
	}

};

class ParticleSystemHolder
{
private:
	vk::VulkanDevice *device;
public:
	std::vector<ParticleSystem*> particleSystems;

	VkPipelineVertexInputStateCreateInfo inputState;
	std::vector<VkVertexInputBindingDescription> bindingDescriptions;
	std::vector<VkVertexInputAttributeDescription> attributeDescriptions;

	ParticleSystemHolder(vk::VulkanDevice *vkdevice) : device(vkdevice)
	{
		// Vertex inputs
		bindingDescriptions = {
			vkTools::initializers::vertexInputBindingDescription(0, sizeof(ParticleSystem::Particle), VK_VERTEX_INPUT_RATE_VERTEX),
		};

		// Attribute descriptions
		// Location 0 : Position
		attributeDescriptions = {
			vkTools::initializers::vertexInputAttributeDescription(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleSystem::Particle, pos)),
			vkTools::initializers::vertexInputAttributeDescription(0, 1, VK_FORMAT_R32G32B32A32_SFLOAT,	offsetof(ParticleSystem::Particle, color)),
			vkTools::initializers::vertexInputAttributeDescription(0, 2, VK_FORMAT_R32_SFLOAT, offsetof(ParticleSystem::Particle, alpha)),
			vkTools::initializers::vertexInputAttributeDescription(0, 3, VK_FORMAT_R32_SFLOAT, offsetof(ParticleSystem::Particle, size)),
			vkTools::initializers::vertexInputAttributeDescription(0, 4, VK_FORMAT_R32_SFLOAT, offsetof(ParticleSystem::Particle, rotation)),
			vkTools::initializers::vertexInputAttributeDescription(0, 5, VK_FORMAT_R32_SINT, offsetof(ParticleSystem::Particle, type)),
		};

		inputState = vkTools::initializers::pipelineVertexInputStateCreateInfo();
		inputState.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescriptions.size());
		inputState.pVertexBindingDescriptions = bindingDescriptions.data();
		inputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
		inputState.pVertexAttributeDescriptions = attributeDescriptions.data();
	};

	~ParticleSystemHolder()
	{
		for (auto& particleSystem : particleSystems)
		{
			delete particleSystem;
		}
	}

	ParticleSystem *add(uint32_t particlecount, glm::vec3 pos, glm::vec3 minvel, glm::vec3 maxvel)
	{
		ParticleSystem *particleSystem;
		particleSystem = new ParticleSystem(device, particlecount, pos, minvel, maxvel);
		particleSystems.push_back(particleSystem);
		return particleSystem;
	}

	void update(float deltaT)
	{
		for (auto& particleSystem : particleSystems)
		{
			particleSystem->updateParticles(deltaT);
		}
	}

};


