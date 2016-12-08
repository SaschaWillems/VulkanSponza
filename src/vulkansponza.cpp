/*
* Vulkan Example - Playground for rendering Crytek's Sponza model (deferred renderer)
*
* Copyright (C) 2016 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <vector>
#include <random>
#include <unordered_map>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <vulkan/vulkan.h>
#include "vulkanexamplebase.h"

#if defined(__ANDROID__)
#include <android/asset_manager.h>
#endif

#define VERTEX_BUFFER_BIND_ID 0
#define ENABLE_VALIDATION false
#define SSAO_KERNEL_SIZE 32
#define SSAO_RADIUS 2.0f
#define SSAO_NOISE_DIM 4

//#define PER_MESH_BUFFERS

// Vertex layout for this example
std::vector<vkMeshLoader::VertexLayout> vertexLayout =
{
	vkMeshLoader::VERTEX_LAYOUT_POSITION,
	vkMeshLoader::VERTEX_LAYOUT_UV,
	vkMeshLoader::VERTEX_LAYOUT_COLOR,
	vkMeshLoader::VERTEX_LAYOUT_NORMAL,
	vkMeshLoader::VERTEX_LAYOUT_TANGENT
};

struct Vertex
{
	glm::vec3 pos;
	glm::vec2 uv;
	glm::vec3 color;
	glm::vec3 normal;
	glm::vec3 tangent;
};

template <typename T> 
class VulkanResourceList
{
public:
	VkDevice &device;
	std::unordered_map<std::string, T> resources;
	VulkanResourceList(VkDevice &dev) : device(dev) {};
	T get(std::string name)
	{
		return resources[name];
	}
};

class PipelineList : public VulkanResourceList<VkPipeline>
{
public:
	PipelineList(VkDevice &dev) : VulkanResourceList(dev) {};

	~PipelineList()
	{
		for (auto& pipeline : resources)
		{
			vkDestroyPipeline(device, pipeline.second, nullptr);
		}
	}

	VkPipeline addGraphicsPipeline(std::string name, VkGraphicsPipelineCreateInfo &pipelineCreateInfo, VkPipelineCache &pipelineCache)
	{
		VkPipeline pipeline;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipeline));
		resources[name] = pipeline;
		return pipeline;
	}
};

PipelineList *pipelineList;

struct SceneMaterial 
{
	std::string name;
	vkTools::VulkanTexture diffuse;
	vkTools::VulkanTexture specular;
	vkTools::VulkanTexture bump;
	bool hasAlpha = false;
	bool hasBump = false;
	bool hasSpecular = false;
	VkPipeline pipeline;
};

struct SceneMesh
{
	VkBuffer vertexBuffer;
	VkDeviceMemory vertexMemory;

	VkBuffer indexBuffer;
	VkDeviceMemory indexMemory;

	uint32_t indexCount;
	uint32_t indexBase;

	// Better move to material and share among meshes with same material
	VkDescriptorSet descriptorSet;

	SceneMaterial *material;
};

VkPhysicalDeviceMemoryProperties deviceMemProps;

uint32_t getMemTypeIndex( uint32_t typeBits, VkFlags properties)
{
	for (uint32_t i = 0; i < 32; i++)
	{
		if ((typeBits & 1) == 1)
		{
			if ((deviceMemProps.memoryTypes[i].propertyFlags & properties) == properties)
			{
				return i;
			}
		}
		typeBits >>= 1;
	}

	// todo: throw if no appropriate mem type was found
	return 0;
}

class Scene
{
private:
	VkDevice device;
	VkQueue queue;
	
	// todo: rename
	vk::Buffer *defaultUBO;
	
	VkDescriptorPool descriptorPool;

	vkTools::VulkanTextureLoader *textureLoader;

	const aiScene* aScene;

	void loadMaterials()
	{
		materials.resize(aScene->mNumMaterials);
		
		for (uint32_t i = 0; i < materials.size(); i++)
		{
			materials[i] = {};

			aiString name;
			aScene->mMaterials[i]->Get(AI_MATKEY_NAME, name);
			aiColor3D ambient;
			aScene->mMaterials[i]->Get(AI_MATKEY_COLOR_AMBIENT, ambient);
			materials[i].name = name.C_Str();
			std::cout << "Material \"" << materials[i].name << "\"" << std::endl;

			// Textures
			aiString texturefile;
			std::string diffuseMapFile;
			// Diffuse
			aScene->mMaterials[i]->GetTexture(aiTextureType_DIFFUSE, 0, &texturefile);
			if (aScene->mMaterials[i]->GetTextureCount(aiTextureType_DIFFUSE) > 0)
			{
				std::cout << "  Diffuse: \"" << texturefile.C_Str() << "\"" << std::endl;
				std::string fileName = std::string(texturefile.C_Str());
				diffuseMapFile = fileName;
				std::replace(fileName.begin(), fileName.end(), '\\', '/');
				textureLoader->loadTexture(assetPath + fileName, VK_FORMAT_BC2_UNORM_BLOCK, &materials[i].diffuse);
			}
			else
			{
				std::cout << "  Material has no diffuse, using dummy texture!" << std::endl;
				textureLoader->loadTexture(assetPath + "sponza/dummy.dds", VK_FORMAT_BC2_UNORM_BLOCK, &materials[i].diffuse);
			}
			// Specular
			if (aScene->mMaterials[i]->GetTextureCount(aiTextureType_SPECULAR) > 0)
			{
				aScene->mMaterials[i]->GetTexture(aiTextureType_SPECULAR, 0, &texturefile);
				std::cout << "  Specular: \"" << texturefile.C_Str() << "\"" << std::endl;
				std::string fileName = std::string(texturefile.C_Str());
				std::replace(fileName.begin(), fileName.end(), '\\', '/');
				textureLoader->loadTexture(assetPath + fileName, VK_FORMAT_BC2_UNORM_BLOCK, &materials[i].specular);
			}
			else
			{
				std::cout << "  Material has no specular, using dummy texture!" << std::endl;
				textureLoader->loadTexture(assetPath + "sponza/dummy_specular.dds", VK_FORMAT_BC2_UNORM_BLOCK, &materials[i].specular);
			}

			// Bump (map_bump is mapped to height by assimp)
			if (aScene->mMaterials[i]->GetTextureCount(aiTextureType_NORMALS) > 0)
			{
				aScene->mMaterials[i]->GetTexture(aiTextureType_NORMALS, 0, &texturefile);
				std::cout << "  Bump: \"" << texturefile.C_Str() << "\"" << std::endl;
				std::string fileName = std::string(texturefile.C_Str());
				std::replace(fileName.begin(), fileName.end(), '\\', '/');
				textureLoader->loadTexture(assetPath + fileName, VK_FORMAT_BC2_UNORM_BLOCK, &materials[i].bump);
				materials[i].hasBump = true;
			}
			else
			{
				std::cout << "  Material has no bump, using dummy texture!" << std::endl;
				textureLoader->loadTexture(assetPath + "sponza/dummy_ddn.dds", VK_FORMAT_BC2_UNORM_BLOCK, &materials[i].bump);
			}

			// Mask
			if (aScene->mMaterials[i]->GetTextureCount(aiTextureType_OPACITY) > 0)
			{
				std::cout << "  Material has opacity, enabling alpha test" << std::endl;
				materials[i].hasAlpha = true;
			}

			materials[i].pipeline = pipelineList->get("scene.solid");
		}

	}

	void loadMeshes(VkCommandBuffer copyCmd)		
	{
		std::vector<Vertex> gVertices;
		std::vector<uint32_t> gIndices;
		uint32_t gIndexBase = 0;

		meshes.resize(aScene->mNumMeshes);
		for (uint32_t i = 0; i < meshes.size(); i++)
		{
			aiMesh *aMesh = aScene->mMeshes[i];

			std::cout << "Mesh \"" << aMesh->mName.C_Str() << "\"" << std::endl;
			std::cout << "	Material: \"" << materials[aMesh->mMaterialIndex].name << "\"" << std::endl;
			std::cout << "	Faces: " << aMesh->mNumFaces << std::endl;
			
			meshes[i].material = &materials[aMesh->mMaterialIndex];
			meshes[i].indexBase = gIndexBase;

			// Vertices
			std::vector<Vertex> vertices;			
			vertices.resize(aMesh->mNumVertices);

			bool hasUV = aMesh->HasTextureCoords(0);
			bool hasTangent = aMesh->HasTangentsAndBitangents();

			uint32_t vertexBase = gVertices.size();

			for (uint32_t i = 0; i < aMesh->mNumVertices; i++)
			{
				vertices[i].pos = glm::make_vec3(&aMesh->mVertices[i].x);// *0.5f;
				vertices[i].pos.y = -vertices[i].pos.y;
				vertices[i].uv = (hasUV) ? glm::make_vec2(&aMesh->mTextureCoords[0][i].x) : glm::vec3(0.0f);
				vertices[i].normal = glm::make_vec3(&aMesh->mNormals[i].x);
				vertices[i].normal.y = -vertices[i].normal.y;
				vertices[i].color = glm::vec3(1.0f); // todo : take from material
				vertices[i].tangent = (hasTangent) ? glm::make_vec3(&aMesh->mTangents[i].x) : glm::vec3(0.0f, 1.0f, 0.0f);
				gVertices.push_back(vertices[i]);
			}

			// Indices
			std::vector<uint32_t> indices;
			meshes[i].indexCount = aMesh->mNumFaces * 3;
			indices.resize(aMesh->mNumFaces * 3);
			for (uint32_t i = 0; i < aMesh->mNumFaces; i++)
			{
				// Assume mesh is triangulated
				indices[i * 3] = aMesh->mFaces[i].mIndices[0];
				indices[i * 3 + 1] = aMesh->mFaces[i].mIndices[1];
				indices[i * 3 + 2] = aMesh->mFaces[i].mIndices[2];
				gIndices.push_back(indices[i*3] + vertexBase);
				gIndices.push_back(indices[i*3+1] + vertexBase);
				gIndices.push_back(indices[i*3+2] + vertexBase);
				gIndexBase += 3;
			}

			// Create buffers
			// todo : staging
			// todo : only one memory allocation

			uint32_t vertexDataSize = vertices.size() * sizeof(Vertex);
			uint32_t indexDataSize = indices.size() * sizeof(uint32_t);

			VkMemoryAllocateInfo memAlloc = vkTools::initializers::memoryAllocateInfo();
			VkMemoryRequirements memReqs;

			VkResult err;
			void *data;

			struct
			{
				struct {
					VkDeviceMemory memory;
					VkBuffer buffer;
				} vBuffer;
				struct {
					VkDeviceMemory memory;
					VkBuffer buffer;
				} iBuffer;
			} staging;

			// Generate vertex buffer
			VkBufferCreateInfo vBufferInfo;

			// Staging buffer
			vBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vertexDataSize);
			VK_CHECK_RESULT(vkCreateBuffer(device, &vBufferInfo, nullptr, &staging.vBuffer.buffer));
			vkGetBufferMemoryRequirements(device, staging.vBuffer.buffer, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &staging.vBuffer.memory));
			VK_CHECK_RESULT(vkMapMemory(device, staging.vBuffer.memory, 0, VK_WHOLE_SIZE, 0, &data));
			memcpy(data, vertices.data(), vertexDataSize);
			vkUnmapMemory(device, staging.vBuffer.memory);
			VK_CHECK_RESULT(vkBindBufferMemory(device, staging.vBuffer.buffer, staging.vBuffer.memory, 0));

			// Target
			vBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, vertexDataSize);
			VK_CHECK_RESULT(vkCreateBuffer(device, &vBufferInfo, nullptr, &meshes[i].vertexBuffer));
			vkGetBufferMemoryRequirements(device, meshes[i].vertexBuffer, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &meshes[i].vertexMemory));
			VK_CHECK_RESULT(vkBindBufferMemory(device, meshes[i].vertexBuffer, meshes[i].vertexMemory, 0));

			// Generate index buffer
			VkBufferCreateInfo iBufferInfo;

			// Staging buffer
			iBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, indexDataSize);
			VK_CHECK_RESULT(vkCreateBuffer(device, &iBufferInfo, nullptr, &staging.iBuffer.buffer));
			vkGetBufferMemoryRequirements(device, staging.iBuffer.buffer, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &staging.iBuffer.memory));
			VK_CHECK_RESULT(vkMapMemory(device, staging.iBuffer.memory, 0, VK_WHOLE_SIZE, 0, &data));
			memcpy(data, indices.data(), indexDataSize);
			vkUnmapMemory(device, staging.iBuffer.memory);
			VK_CHECK_RESULT(vkBindBufferMemory(device, staging.iBuffer.buffer, staging.iBuffer.memory, 0));

			// Target
			iBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, indexDataSize);
			VK_CHECK_RESULT(vkCreateBuffer(device, &iBufferInfo, nullptr, &meshes[i].indexBuffer));
			vkGetBufferMemoryRequirements(device, meshes[i].indexBuffer, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &meshes[i].indexMemory));
			VK_CHECK_RESULT(vkBindBufferMemory(device, meshes[i].indexBuffer, meshes[i].indexMemory, 0));

			// Copy
			VkCommandBufferBeginInfo cmdBufInfo = vkTools::initializers::commandBufferBeginInfo();
			VK_CHECK_RESULT(vkBeginCommandBuffer(copyCmd, &cmdBufInfo));

			VkBufferCopy copyRegion = {};

			copyRegion.size = vertexDataSize;
			vkCmdCopyBuffer(
				copyCmd,
				staging.vBuffer.buffer,
				meshes[i].vertexBuffer,
				1,
				&copyRegion);

			copyRegion.size = indexDataSize;
			vkCmdCopyBuffer(
				copyCmd,
				staging.iBuffer.buffer,
				meshes[i].indexBuffer,
				1,
				&copyRegion);

			VK_CHECK_RESULT(vkEndCommandBuffer(copyCmd));
			
			VkSubmitInfo submitInfo = {};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submitInfo.commandBufferCount = 1;
			submitInfo.pCommandBuffers = &copyCmd;

			VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
			VK_CHECK_RESULT(vkQueueWaitIdle(queue));

			vkDestroyBuffer(device, staging.vBuffer.buffer, nullptr);
			vkFreeMemory(device, staging.vBuffer.memory, nullptr);
			vkDestroyBuffer(device, staging.iBuffer.buffer, nullptr);
			vkFreeMemory(device, staging.iBuffer.memory, nullptr);
		}

		/* test: global buffers */

		size_t vertexDataSize = gVertices.size() * sizeof(Vertex);
		size_t indexDataSize = gIndices.size() * sizeof(uint32_t);

		VkMemoryAllocateInfo memAlloc = vkTools::initializers::memoryAllocateInfo();
		VkMemoryRequirements memReqs;

		VkResult err;
		void *data;

		struct
		{
			struct {
				VkDeviceMemory memory;
				VkBuffer buffer;
			} vBuffer;
			struct {
				VkDeviceMemory memory;
				VkBuffer buffer;
			} iBuffer;
		} staging;

		// Generate vertex buffer
		VkBufferCreateInfo vBufferInfo;

		// Staging buffer
		vBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vertexDataSize);
		VK_CHECK_RESULT(vkCreateBuffer(device, &vBufferInfo, nullptr, &staging.vBuffer.buffer));
		vkGetBufferMemoryRequirements(device, staging.vBuffer.buffer, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = getMemTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &staging.vBuffer.memory));
		VK_CHECK_RESULT(vkMapMemory(device, staging.vBuffer.memory, 0, VK_WHOLE_SIZE, 0, &data));
		memcpy(data, gVertices.data(), vertexDataSize);
		vkUnmapMemory(device, staging.vBuffer.memory);
		VK_CHECK_RESULT(vkBindBufferMemory(device, staging.vBuffer.buffer, staging.vBuffer.memory, 0));

		// Target
		vBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, vertexDataSize);
		VK_CHECK_RESULT(vkCreateBuffer(device, &vBufferInfo, nullptr, &vertexBuffer.buffer));
		vkGetBufferMemoryRequirements(device, vertexBuffer.buffer, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = getMemTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &vertexBuffer.memory));
		VK_CHECK_RESULT(vkBindBufferMemory(device, vertexBuffer.buffer, vertexBuffer.memory, 0));

		// Generate index buffer
		VkBufferCreateInfo iBufferInfo;

		// Staging buffer
		iBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, indexDataSize);
		VK_CHECK_RESULT(vkCreateBuffer(device, &iBufferInfo, nullptr, &staging.iBuffer.buffer));
		vkGetBufferMemoryRequirements(device, staging.iBuffer.buffer, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = getMemTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &staging.iBuffer.memory));
		VK_CHECK_RESULT(vkMapMemory(device, staging.iBuffer.memory, 0, VK_WHOLE_SIZE, 0, &data));
		memcpy(data, gIndices.data(), indexDataSize);
		vkUnmapMemory(device, staging.iBuffer.memory);
		VK_CHECK_RESULT(vkBindBufferMemory(device, staging.iBuffer.buffer, staging.iBuffer.memory, 0));

		// Target
		iBufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, indexDataSize);
		VK_CHECK_RESULT(vkCreateBuffer(device, &iBufferInfo, nullptr, &indexBuffer.buffer));
		vkGetBufferMemoryRequirements(device, indexBuffer.buffer, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = getMemTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &indexBuffer.memory));
		VK_CHECK_RESULT(vkBindBufferMemory(device, indexBuffer.buffer, indexBuffer.memory, 0));

		// Copy
		VkCommandBufferBeginInfo cmdBufInfo = vkTools::initializers::commandBufferBeginInfo();
		VK_CHECK_RESULT(vkBeginCommandBuffer(copyCmd, &cmdBufInfo));

		VkBufferCopy copyRegion = {};

		copyRegion.size = vertexDataSize;
		vkCmdCopyBuffer(
			copyCmd,
			staging.vBuffer.buffer,
			vertexBuffer.buffer,
			1,
			&copyRegion);

		copyRegion.size = indexDataSize;
		vkCmdCopyBuffer(
			copyCmd,
			staging.iBuffer.buffer,
			indexBuffer.buffer,
			1,
			&copyRegion);

		VK_CHECK_RESULT(vkEndCommandBuffer(copyCmd));

		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &copyCmd;

		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
		VK_CHECK_RESULT(vkQueueWaitIdle(queue));

		vkDestroyBuffer(device, staging.vBuffer.buffer, nullptr);
		vkFreeMemory(device, staging.vBuffer.memory, nullptr);
		vkDestroyBuffer(device, staging.iBuffer.buffer, nullptr);
		vkFreeMemory(device, staging.iBuffer.memory, nullptr);

		// Generate descriptor sets for all meshes
		// todo : think about a nicer solution, better suited per material?

		// Decriptor pool
		std::vector<VkDescriptorPoolSize> poolSizes;
		poolSizes.push_back(vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, meshes.size()));
		poolSizes.push_back(vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, meshes.size() * 3));

		VkDescriptorPoolCreateInfo descriptorPoolInfo =
			vkTools::initializers::descriptorPoolCreateInfo(
				poolSizes.size(),
				poolSizes.data(),
				meshes.size());

		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));

		// Shared descriptor set layout
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;
		// Binding 0: UBO
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_SHADER_STAGE_VERTEX_BIT,
			0));
		// Binding 1: Diffuse map
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			1));
		// Binding 2: Specular map
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			2));
		// Binding 3: Bump map
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			3));

		VkDescriptorSetLayoutCreateInfo descriptorLayout =
			vkTools::initializers::descriptorSetLayoutCreateInfo(
				setLayoutBindings.data(),
				setLayoutBindings.size());

		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayout));

		VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo =
			vkTools::initializers::pipelineLayoutCreateInfo(
				&descriptorSetLayout,
				1);

		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pPipelineLayoutCreateInfo, nullptr, &pipelineLayout));

		// Descriptor sets
		for (uint32_t i = 0; i < meshes.size(); i++)
		{
			// Descriptor set
			VkDescriptorSetAllocateInfo allocInfo =
				vkTools::initializers::descriptorSetAllocateInfo(
					descriptorPool,
					&descriptorSetLayout,
					1);

			// Background
			VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &meshes[i].descriptorSet));

			std::vector<VkWriteDescriptorSet> writeDescriptorSets;

			// Binding 0 : Vertex shader uniform buffer
			writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
				meshes[i].descriptorSet,
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				0,
				&defaultUBO->descriptor));
			// Image bindings
			// Binding 0: Color map
			writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
				meshes[i].descriptorSet,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				1,
				&meshes[i].material->diffuse.descriptor));
			// Binding 1: Specular
			writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
				meshes[i].descriptorSet,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				2,
				&meshes[i].material->specular.descriptor));
			// Binding 2: Normal
			writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
				meshes[i].descriptorSet,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				3,
				&meshes[i].material->bump.descriptor));

			vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);
		}
	}

public:
#if defined(__ANDROID__)
	AAssetManager* assetManager = nullptr;
#endif

	std::string assetPath = "";

	std::vector<SceneMaterial> materials;
	std::vector<SceneMesh> meshes;

	vk::Buffer vertexBuffer;
	vk::Buffer indexBuffer;

	// Same for all meshes in the scene
	VkDescriptorSetLayout descriptorSetLayout;
	VkPipelineLayout pipelineLayout;

	Scene(VkDevice device, VkQueue queue, vkTools::VulkanTextureLoader *textureloader, vk::Buffer *defaultUBO)
	{
		this->device = device;
		this->queue = queue;
		this->textureLoader = textureloader;
		this->defaultUBO = defaultUBO;
	}

	~Scene()
	{
		for (auto mesh : meshes)
		{
			vkDestroyBuffer(device, mesh.vertexBuffer, nullptr);
			vkFreeMemory(device, mesh.vertexMemory, nullptr);
			vkDestroyBuffer(device, mesh.indexBuffer, nullptr);
			vkFreeMemory(device, mesh.indexMemory, nullptr);
		}
		for (auto material : materials)
		{
			textureLoader->destroyTexture(material.diffuse);
			textureLoader->destroyTexture(material.bump);
		}
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
		vkDestroyDescriptorPool(device, descriptorPool, nullptr);
	}

	void load(std::string filename, VkCommandBuffer copyCmd)
	{
		Assimp::Importer Importer;

		int flags = aiProcess_FlipWindingOrder | aiProcess_Triangulate | aiProcess_PreTransformVertices | aiProcess_CalcTangentSpace | aiProcess_GenSmoothNormals;

#if defined(__ANDROID__)
		AAsset* asset = AAssetManager_open(assetManager, filename.c_str(), AASSET_MODE_STREAMING);
		assert(asset);
		size_t size = AAsset_getLength(asset);
		assert(size > 0);
		void *meshData = malloc(size);
		AAsset_read(asset, meshData, size);
		AAsset_close(asset);
		aScene = Importer.ReadFileFromMemory(meshData, size, flags);
		free(meshData);
#else
		aScene = Importer.ReadFile(filename.c_str(), flags);
#endif
		if (aScene)
		{
			loadMaterials();
			loadMeshes(copyCmd);
		}
		else
		{
			printf("Error parsing '%s': '%s'\n", filename.c_str(), Importer.GetErrorString());
#if defined(__ANDROID__)
			LOGE("Error parsing '%s': '%s'", filename.c_str(), Importer.GetErrorString());
#endif
		}
	}
};

class VulkanExample : public VulkanExampleBase
{
public:
	Scene *scene;

	bool debugDisplay = false;
	bool attachLight = false;
	bool enableSSAO = true;

	// Vendor specific
	bool enableNVDedicatedAllocation = false;

	struct {
		vkTools::VulkanTexture colorMap;
		vkTools::VulkanTexture ssaoNoise;
	} textures;

	struct {
		vkMeshLoader::MeshBuffer quad;
	} meshes;

	struct {
		VkPipelineVertexInputStateCreateInfo inputState;
		std::vector<VkVertexInputBindingDescription> bindingDescriptions;
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions;
	} vertices;

	struct {
		glm::mat4 projection;
		glm::mat4 model;
		glm::mat4 view;
	} uboVS, uboSceneMatrices;

	struct UBOSSAOParams {
		glm::mat4 projection;
		uint32_t ssao = true;
		uint32_t ssaoOnly = false;
		uint32_t ssaoBlur = true;
	} uboSSAOParams;

	struct Light {
		glm::vec4 position;
		glm::vec4 color;
		float radius;
		float quadraticFalloff;
		float linearFalloff;
		float _pad;
	};

	struct {
		Light lights[17];
		glm::vec4 viewPos;
		glm::mat4 view;
		glm::mat4 model;
	} uboFragmentLights;

	struct {
		vk::Buffer fullScreen;
		vk::Buffer sceneMatrices;
		vk::Buffer sceneLights;
		vk::Buffer ssaoKernel;
		vk::Buffer ssaoParams;
	} uniformBuffers;

	struct {
		VkPipelineLayout offscreen;
		VkPipelineLayout ssao;
		VkPipelineLayout ssaoBlur;
		VkPipelineLayout composition;
	} pipelineLayouts;

	struct {
		VkDescriptorSet offscreen;
		VkDescriptorSet ssao;
		VkDescriptorSet ssaoBlur;
		VkDescriptorSet composition;
	} descriptorSets;

	struct {
		VkDescriptorSetLayout offscreen;
		VkDescriptorSetLayout ssao;
		VkDescriptorSetLayout ssaoBlur;
		VkDescriptorSetLayout composition;
	} descriptorSetLayouts;

	// Framebuffer for offscreen rendering
	struct FrameBufferAttachment {
		VkImage image;
		VkDeviceMemory mem;
		VkImageView view;
		VkFormat format;
		void destroy(VkDevice device)
		{
			vkDestroyImage(device, image, nullptr);
			vkDestroyImageView(device, view, nullptr);
			vkFreeMemory(device, mem, nullptr);
		}
	};
	struct FrameBuffer {
		int32_t width, height;
		VkFramebuffer frameBuffer;
		FrameBufferAttachment depth;
		VkRenderPass renderPass;
		void setSize(int32_t w, int32_t h)
		{
			this->width = w;
			this->height = h;
		}
		void destroy(VkDevice device)
		{
			vkDestroyFramebuffer(device, frameBuffer, nullptr);
			vkDestroyRenderPass(device, renderPass, nullptr);
		}
	};
	

	struct {
		struct Offscreen : public FrameBuffer {
			std::array<FrameBufferAttachment, 3> attachments;
		} offscreen;
		struct SSAO : public FrameBuffer {
			std::array<FrameBufferAttachment, 1 > attachments;
		} ssao, ssaoBlur;
	} frameBuffers;
	
	// One sampler for the frame buffer color attachments
	VkSampler colorSampler;

	VkCommandBuffer offScreenCmdBuffer = VK_NULL_HANDLE;

	// Semaphore used to synchronize between offscreen and final scene rendering
	VkSemaphore offscreenSemaphore = VK_NULL_HANDLE;

	VulkanExample() : VulkanExampleBase(ENABLE_VALIDATION)
	{
#if !defined(__ANDROID__)
		width = 1920;
		height = 1080;
#endif
		enableTextOverlay = true;
		title = "Vulkan Sponza - (c) 2016 by Sascha Willems";

		camera.type = Camera::CameraType::firstperson;
		camera.setPerspective(60.0f, (float)width / (float)height, 1.0f, 256.0f);
		camera.setRotation(glm::vec3(7.0f, -75.0f, 0.0f));
		camera.setTranslation(glm::vec3(0.0f, 2.5f, 0.0f));
		camera.movementSpeed = 20.0f * 2.0f;

		timerSpeed = 0.075f;
		rotationSpeed = 0.15f;
#if defined(_WIN32)
		setupConsole("VulkanExample");
#endif
		srand(time(NULL));

		enableNVDedicatedAllocation = vulkanDevice->extensionSupported(VK_NV_DEDICATED_ALLOCATION_EXTENSION_NAME);

		pipelineList = new PipelineList(vulkanDevice->logicalDevice);
	}

	~VulkanExample()
	{
		delete pipelineList;

		vkDestroySampler(device, colorSampler, nullptr);

		// Frame buffer attachments
		for (auto attachment : frameBuffers.offscreen.attachments)
		{
			vkDestroyImageView(device, attachment.view, nullptr);
			vkDestroyImage(device, attachment.image, nullptr);
			vkFreeMemory(device, attachment.mem, nullptr);
		}

		// Depth attachment
		vkDestroyImageView(device, frameBuffers.offscreen.depth.view, nullptr);
		vkDestroyImage(device, frameBuffers.offscreen.depth.image, nullptr);
		vkFreeMemory(device, frameBuffers.offscreen.depth.mem, nullptr);

		vkDestroyFramebuffer(device, frameBuffers.offscreen.frameBuffer, nullptr);

		vkDestroyPipelineLayout(device, pipelineLayouts.composition, nullptr);
		vkDestroyPipelineLayout(device, pipelineLayouts.offscreen, nullptr);

		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.composition, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.offscreen, nullptr);

		// Meshes
		vkMeshLoader::freeMeshBufferResources(device, &meshes.quad);

		// Uniform buffers
		uniformBuffers.fullScreen.destroy();
		uniformBuffers.sceneMatrices.destroy();
		uniformBuffers.sceneLights.destroy();

		vkFreeCommandBuffers(device, cmdPool, 1, &offScreenCmdBuffer);

		vkDestroyRenderPass(device, frameBuffers.offscreen.renderPass, nullptr);

		textureLoader->destroyTexture(textures.colorMap);

		vkDestroySemaphore(device, offscreenSemaphore, nullptr);

		delete(scene);
	}

	// Create a frame buffer attachment
	void createAttachment(
		VkFormat format,
		VkImageUsageFlagBits usage,
		FrameBufferAttachment *attachment,
		VkCommandBuffer layoutCmd,
		uint32_t width,
		uint32_t height)
	{
		VkImageAspectFlags aspectMask = 0;
		VkImageLayout imageLayout;

		attachment->format = format;

		if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
		{
			aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}
		if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
		{
			aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		}

		assert(aspectMask > 0);

		VkImageCreateInfo image = vkTools::initializers::imageCreateInfo();
		image.imageType = VK_IMAGE_TYPE_2D;
		image.format = format;
		image.extent.width = width;
		image.extent.height = height;
		image.extent.depth = 1;
		image.mipLevels = 1;
		image.arrayLayers = 1;
		image.samples = VK_SAMPLE_COUNT_1_BIT;
		image.tiling = VK_IMAGE_TILING_OPTIMAL;
		image.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT;

		if (enableNVDedicatedAllocation)
		{
			VkDedicatedAllocationImageCreateInfoNV dedicatedImageInfo { VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV };
			dedicatedImageInfo.dedicatedAllocation = VK_TRUE;
			image.pNext = &dedicatedImageInfo;
		}
		VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &attachment->image));

		VkMemoryAllocateInfo memAlloc = vkTools::initializers::memoryAllocateInfo();
		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(device, attachment->image, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = getMemTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		if (enableNVDedicatedAllocation)
		{
			VkDedicatedAllocationMemoryAllocateInfoNV dedicatedAllocationInfo { VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_MEMORY_ALLOCATE_INFO_NV };
			dedicatedAllocationInfo.image = attachment->image;
			memAlloc.pNext = &dedicatedAllocationInfo;
		}

		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &attachment->mem));
		VK_CHECK_RESULT(vkBindImageMemory(device, attachment->image, attachment->mem, 0));

		VkImageViewCreateInfo imageView = vkTools::initializers::imageViewCreateInfo();
		imageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageView.format = format;
		imageView.subresourceRange = {};
		imageView.subresourceRange.aspectMask = aspectMask;
		imageView.subresourceRange.baseMipLevel = 0;
		imageView.subresourceRange.levelCount = 1;
		imageView.subresourceRange.baseArrayLayer = 0;
		imageView.subresourceRange.layerCount = 1;
		imageView.image = attachment->image;
		VK_CHECK_RESULT(vkCreateImageView(device, &imageView, nullptr, &attachment->view));
	}

	// Prepare a new framebuffer for offscreen rendering
	// The contents of this framebuffer are then
	// blitted to our render target
	void prepareOffscreenFramebuffers()
	{
		VkCommandBuffer layoutCmd = VulkanExampleBase::createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);

#if defined(__ANDROID__)
		const uint32_t ssaoWidth = width / 2;
		const uint32_t ssaoHeight = height / 2;
#else
		const uint32_t ssaoWidth = width;
		const uint32_t ssaoHeight = height;
#endif

		frameBuffers.offscreen.setSize(width, height);
		frameBuffers.ssao.setSize(ssaoWidth, ssaoHeight);
		frameBuffers.ssaoBlur.setSize(width, height);

		// Color attachments
		// Attachment 0: World space positions
		createAttachment(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &frameBuffers.offscreen.attachments[0], layoutCmd, width, height);

		// Attachment 1: World space normal
		createAttachment(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &frameBuffers.offscreen.attachments[1], layoutCmd, width, height);

		// Attachment 1: Packed colors, specular
		createAttachment(VK_FORMAT_R32G32B32A32_UINT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &frameBuffers.offscreen.attachments[2], layoutCmd, width, height);

		// Depth attachment

		// Find a suitable depth format
		VkFormat attDepthFormat;
		VkBool32 validDepthFormat = vkTools::getSupportedDepthFormat(physicalDevice, &attDepthFormat);
		assert(validDepthFormat);

		createAttachment(attDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &frameBuffers.offscreen.depth, layoutCmd, width, height);

		// SSAO
		createAttachment(VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &frameBuffers.ssao.attachments[0], layoutCmd, ssaoWidth, ssaoHeight);				// Color																																				
		// SSAO blur
		createAttachment(VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &frameBuffers.ssaoBlur.attachments[0], layoutCmd, width, height);					// Color

		VulkanExampleBase::flushCommandBuffer(layoutCmd, queue, true);

		// G-Buffer creation
		{
			std::array<VkAttachmentDescription, 4> attachmentDescs = {};

			// Init attachment properties
			for (uint32_t i = 0; i < static_cast<uint32_t>(attachmentDescs.size()); i++)
			{
				attachmentDescs[i].samples = VK_SAMPLE_COUNT_1_BIT;
				attachmentDescs[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				attachmentDescs[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				attachmentDescs[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
				attachmentDescs[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
				attachmentDescs[i].finalLayout = (i == 3) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			}

			// Formats
			attachmentDescs[0].format = frameBuffers.offscreen.attachments[0].format;
			attachmentDescs[1].format = frameBuffers.offscreen.attachments[1].format;
			attachmentDescs[2].format = frameBuffers.offscreen.attachments[2].format;
			attachmentDescs[3].format = frameBuffers.offscreen.depth.format;

			std::vector<VkAttachmentReference> colorReferences;
			colorReferences.push_back({ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
			colorReferences.push_back({ 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
			colorReferences.push_back({ 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });

			VkAttachmentReference depthReference = {};
			depthReference.attachment = 3;
			depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkSubpassDescription subpass = {};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.pColorAttachments = colorReferences.data();
			subpass.colorAttachmentCount = static_cast<uint32_t>(colorReferences.size());
			subpass.pDepthStencilAttachment = &depthReference;

			// Use subpass dependencies for attachment layout transitions
			std::array<VkSubpassDependency, 2> dependencies;

			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			VkRenderPassCreateInfo renderPassInfo = {};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.pAttachments = attachmentDescs.data();
			renderPassInfo.attachmentCount = static_cast<uint32_t>(attachmentDescs.size());
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = 2;
			renderPassInfo.pDependencies = dependencies.data();
			VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassInfo, nullptr, &frameBuffers.offscreen.renderPass));

			std::array<VkImageView, 4> attachments;
			attachments[0] = frameBuffers.offscreen.attachments[0].view;
			attachments[1] = frameBuffers.offscreen.attachments[1].view;
			attachments[2] = frameBuffers.offscreen.attachments[2].view;
			attachments[3] = frameBuffers.offscreen.depth.view;

			VkFramebufferCreateInfo fbufCreateInfo = vkTools::initializers::framebufferCreateInfo();
			fbufCreateInfo.renderPass = frameBuffers.offscreen.renderPass;
			fbufCreateInfo.pAttachments = attachments.data();
			fbufCreateInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			fbufCreateInfo.width = frameBuffers.offscreen.width;
			fbufCreateInfo.height = frameBuffers.offscreen.height;
			fbufCreateInfo.layers = 1;
			VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &frameBuffers.offscreen.frameBuffer));
		}

		// SSAO 
		{
			VkAttachmentDescription attachmentDescription{};
			attachmentDescription.format = frameBuffers.ssao.attachments[0].format;
			attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT;
			attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachmentDescription.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachmentDescription.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

			VkSubpassDescription subpass = {};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.pColorAttachments = &colorReference;
			subpass.colorAttachmentCount = 1;

			std::array<VkSubpassDependency, 2> dependencies;

			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			VkRenderPassCreateInfo renderPassInfo = {};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.pAttachments = &attachmentDescription;
			renderPassInfo.attachmentCount = 1;
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = 2;
			renderPassInfo.pDependencies = dependencies.data();
			VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassInfo, nullptr, &frameBuffers.ssao.renderPass));

			VkFramebufferCreateInfo fbufCreateInfo = vkTools::initializers::framebufferCreateInfo();
			fbufCreateInfo.renderPass = frameBuffers.ssao.renderPass;
			fbufCreateInfo.pAttachments = &frameBuffers.ssao.attachments[0].view;
			fbufCreateInfo.attachmentCount = 1;
			fbufCreateInfo.width = frameBuffers.ssao.width;
			fbufCreateInfo.height = frameBuffers.ssao.height;
			fbufCreateInfo.layers = 1;
			VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &frameBuffers.ssao.frameBuffer));
		}

		// SSAO Blur 
		{
			VkAttachmentDescription attachmentDescription{};
			attachmentDescription.format = frameBuffers.ssao.attachments[0].format;
			attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT;
			attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachmentDescription.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachmentDescription.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

			VkSubpassDescription subpass = {};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.pColorAttachments = &colorReference;
			subpass.colorAttachmentCount = 1;

			std::array<VkSubpassDependency, 2> dependencies;

			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			VkRenderPassCreateInfo renderPassInfo = {};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.pAttachments = &attachmentDescription;
			renderPassInfo.attachmentCount = 1;
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = 2;
			renderPassInfo.pDependencies = dependencies.data();
			VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassInfo, nullptr, &frameBuffers.ssaoBlur.renderPass));

			VkFramebufferCreateInfo fbufCreateInfo = vkTools::initializers::framebufferCreateInfo();
			fbufCreateInfo.renderPass = frameBuffers.ssaoBlur.renderPass;
			fbufCreateInfo.pAttachments = &frameBuffers.ssaoBlur.attachments[0].view;
			fbufCreateInfo.attachmentCount = 1;
			fbufCreateInfo.width = frameBuffers.ssaoBlur.width;
			fbufCreateInfo.height = frameBuffers.ssaoBlur.height;
			fbufCreateInfo.layers = 1;
			VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &frameBuffers.ssaoBlur.frameBuffer));
		}

		// Shared sampler for color attachments
		VkSamplerCreateInfo sampler = vkTools::initializers::samplerCreateInfo();
		sampler.magFilter = VK_FILTER_LINEAR;
		sampler.minFilter = VK_FILTER_LINEAR;
		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.addressModeV = sampler.addressModeU;
		sampler.addressModeW = sampler.addressModeU;
		sampler.mipLodBias = 0.0f;
		sampler.maxAnisotropy = 0;
		sampler.minLod = 0.0f;
		sampler.maxLod = 1.0f;
		sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		VK_CHECK_RESULT(vkCreateSampler(device, &sampler, nullptr, &colorSampler));
	}

	// Build command buffer for rendering the scene to the offscreen frame buffer 
	// and blitting it to the different texture targets
	void buildDeferredCommandBuffer(bool rebuild = false)
	{

		if ((offScreenCmdBuffer == VK_NULL_HANDLE) || (rebuild))
		{
			if (rebuild)
			{
				vkFreeCommandBuffers(device, cmdPool, 1, &offScreenCmdBuffer);
			}
			offScreenCmdBuffer = VulkanExampleBase::createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
		}

		// Create a semaphore used to synchronize offscreen rendering and usage
		VkSemaphoreCreateInfo semaphoreCreateInfo = vkTools::initializers::semaphoreCreateInfo();
		VK_CHECK_RESULT(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &offscreenSemaphore));

		VkCommandBufferBeginInfo cmdBufInfo = vkTools::initializers::commandBufferBeginInfo();

		// Clear values for all attachments written in the fragment sahder
		std::array<VkClearValue, 4> clearValues = {};
		clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
		clearValues[1].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
		clearValues[2].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
		clearValues[3].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo renderPassBeginInfo = vkTools::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderPass = frameBuffers.offscreen.renderPass;
		renderPassBeginInfo.framebuffer = frameBuffers.offscreen.frameBuffer;
		renderPassBeginInfo.renderArea.extent.width = frameBuffers.offscreen.width;
		renderPassBeginInfo.renderArea.extent.height = frameBuffers.offscreen.height;
		renderPassBeginInfo.clearValueCount = clearValues.size();
		renderPassBeginInfo.pClearValues = clearValues.data();

		VK_CHECK_RESULT(vkBeginCommandBuffer(offScreenCmdBuffer, &cmdBufInfo));

		// First pass: Fill G-Buffer components (positions+depth, normals, albedo) using MRT
		// -------------------------------------------------------------------------------------------------------

		vkCmdBeginRenderPass(offScreenCmdBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport = vkTools::initializers::viewport(
			(float)frameBuffers.offscreen.width,
			(float)frameBuffers.offscreen.height,
			0.0f,
			1.0f);
		vkCmdSetViewport(offScreenCmdBuffer, 0, 1, &viewport);

		VkRect2D scissor = vkTools::initializers::rect2D(
			frameBuffers.offscreen.width,
			frameBuffers.offscreen.height,
			0,
			0);
		vkCmdSetScissor(offScreenCmdBuffer, 0, 1, &scissor);

		vkCmdBindPipeline(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineList->get("scene.solid"));

		VkDeviceSize offsets[1] = { 0 };

#ifdef PER_MESH_BUFFERS
		// Render using separate buffers
		for (auto mesh : scene->meshes)
		{
			if (mesh.material->hasAlpha)
			{
				continue;
			}
			vkCmdBindDescriptorSets(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelineLayout, 0, 1, &mesh.descriptorSet, 0, NULL);
			vkCmdBindVertexBuffers(offScreenCmdBuffer, VERTEX_BUFFER_BIND_ID, 1, &mesh.vertexBuffer, offsets);
			vkCmdBindIndexBuffer(offScreenCmdBuffer, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexed(offScreenCmdBuffer, mesh.indexCount, 1, 0, 0, 0);
		}

		vkCmdBindPipeline(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineList->get("scene.blend"));

		for (auto mesh : scene->meshes)
		{
			if (mesh.material->hasAlpha)
			{
				vkCmdBindDescriptorSets(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelineLayout, 0, 1, &mesh.descriptorSet, 0, NULL);
				vkCmdBindVertexBuffers(offScreenCmdBuffer, VERTEX_BUFFER_BIND_ID, 1, &mesh.vertexBuffer, offsets);
				vkCmdBindIndexBuffer(offScreenCmdBuffer, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
				vkCmdDrawIndexed(offScreenCmdBuffer, mesh.indexCount, 1, 0, 0, 0);
			}
		}

#else
		// Render from global buffer using index offsets
		vkCmdBindVertexBuffers(offScreenCmdBuffer, VERTEX_BUFFER_BIND_ID, 1, &scene->vertexBuffer.buffer, offsets);
		vkCmdBindIndexBuffer(offScreenCmdBuffer, scene->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

		for (auto mesh : scene->meshes)
		{
			if (mesh.material->hasAlpha)
			{
				continue;
			}
			vkCmdBindDescriptorSets(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelineLayout, 0, 1, &mesh.descriptorSet, 0, NULL);
			vkCmdDrawIndexed(offScreenCmdBuffer, mesh.indexCount, 1, 0, mesh.indexBase, 0);
		}

		vkCmdBindPipeline(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineList->get("scene.blend"));

		for (auto mesh : scene->meshes)
		{
			if (mesh.material->hasAlpha)
			{
				vkCmdBindDescriptorSets(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelineLayout, 0, 1, &mesh.descriptorSet, 0, NULL);
				vkCmdDrawIndexed(offScreenCmdBuffer, mesh.indexCount, 1, 0, mesh.indexBase, 0);
			}
		}


#endif

		vkCmdEndRenderPass(offScreenCmdBuffer);

		if (enableSSAO)
		{

			// Second pass: SSAO generation
			// -------------------------------------------------------------------------------------------------------

			clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
			clearValues[1].depthStencil = { 1.0f, 0 };

			renderPassBeginInfo.framebuffer = frameBuffers.ssao.frameBuffer;
			renderPassBeginInfo.renderPass = frameBuffers.ssao.renderPass;
			renderPassBeginInfo.renderArea.extent.width = frameBuffers.ssao.width;
			renderPassBeginInfo.renderArea.extent.height = frameBuffers.ssao.height;
			renderPassBeginInfo.clearValueCount = 2;
			renderPassBeginInfo.pClearValues = clearValues.data();

			vkCmdBeginRenderPass(offScreenCmdBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

			viewport = vkTools::initializers::viewport((float)frameBuffers.ssao.width, (float)frameBuffers.ssao.height, 0.0f, 1.0f);
			vkCmdSetViewport(offScreenCmdBuffer, 0, 1, &viewport);
			scissor = vkTools::initializers::rect2D(frameBuffers.ssao.width, frameBuffers.ssao.height, 0, 0);
			vkCmdSetScissor(offScreenCmdBuffer, 0, 1, &scissor);

			vkCmdBindDescriptorSets(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.ssao, 0, 1, &descriptorSets.ssao, 0, NULL);
			vkCmdBindPipeline(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineList->get("ssao.generate"));
			vkCmdDraw(offScreenCmdBuffer, 3, 1, 0, 0);

			vkCmdEndRenderPass(offScreenCmdBuffer);

			// Third pass: SSAO blur
			// -------------------------------------------------------------------------------------------------------

			renderPassBeginInfo.framebuffer = frameBuffers.ssaoBlur.frameBuffer;
			renderPassBeginInfo.renderPass = frameBuffers.ssaoBlur.renderPass;
			renderPassBeginInfo.renderArea.extent.width = frameBuffers.ssaoBlur.width;
			renderPassBeginInfo.renderArea.extent.height = frameBuffers.ssaoBlur.height;

			vkCmdBeginRenderPass(offScreenCmdBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

			viewport = vkTools::initializers::viewport((float)frameBuffers.ssaoBlur.width, (float)frameBuffers.ssaoBlur.height, 0.0f, 1.0f);
			vkCmdSetViewport(offScreenCmdBuffer, 0, 1, &viewport);
			scissor = vkTools::initializers::rect2D(frameBuffers.ssaoBlur.width, frameBuffers.ssaoBlur.height, 0, 0);
			vkCmdSetScissor(offScreenCmdBuffer, 0, 1, &scissor);

			vkCmdBindDescriptorSets(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.ssaoBlur, 0, 1, &descriptorSets.ssaoBlur, 0, NULL);
			vkCmdBindPipeline(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineList->get("ssao.blur"));
			vkCmdDraw(offScreenCmdBuffer, 3, 1, 0, 0);

			vkCmdEndRenderPass(offScreenCmdBuffer);
		}

		VK_CHECK_RESULT(vkEndCommandBuffer(offScreenCmdBuffer));
	}

	void loadTextures()
	{
		textureLoader->loadTexture(
			getAssetPath() + "sponza/background.dds",
			VK_FORMAT_BC2_UNORM_BLOCK,
			&textures.colorMap);
	}

	void reBuildCommandBuffers()
	{
		vkDeviceWaitIdle(device);
		if (!checkCommandBuffers())
		{
			destroyCommandBuffers();
			createCommandBuffers();
		}
		buildCommandBuffers();
	}

	void buildCommandBuffers()
	{
		VkCommandBufferBeginInfo cmdBufInfo = vkTools::initializers::commandBufferBeginInfo();

		VkClearValue clearValues[2];
		clearValues[0].color = { { 0.0f, 0.0f, 0.2f, 0.0f } };
		clearValues[1].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo renderPassBeginInfo = vkTools::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderPass = renderPass;
		renderPassBeginInfo.renderArea.offset.x = 0;
		renderPassBeginInfo.renderArea.offset.y = 0;
		renderPassBeginInfo.renderArea.extent.width = width;
		renderPassBeginInfo.renderArea.extent.height = height;
		renderPassBeginInfo.clearValueCount = 2;
		renderPassBeginInfo.pClearValues = clearValues;

		VkResult err;

		for (int32_t i = 0; i < drawCmdBuffers.size(); ++i)
		{
			// Set target frame buffer
			renderPassBeginInfo.framebuffer = VulkanExampleBase::frameBuffers[i];

			VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));

			vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

			VkViewport viewport = vkTools::initializers::viewport(
				(float)width,
				(float)height,
				0.0f,
				1.0f);
			vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

			VkRect2D scissor = vkTools::initializers::rect2D(
				width,
				height,
				0,
				0);
			vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

			VkDeviceSize offsets[1] = { 0 };
			vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.composition, 0, 1, &descriptorSets.composition, 0, NULL);

			if (debugDisplay)
			{
				vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineList->get("debugdisplay"));
				vkCmdBindVertexBuffers(drawCmdBuffers[i], VERTEX_BUFFER_BIND_ID, 1, &meshes.quad.vertices.buf, offsets);
				vkCmdBindIndexBuffer(drawCmdBuffers[i], meshes.quad.indices.buf, 0, VK_INDEX_TYPE_UINT32);
				vkCmdDrawIndexed(drawCmdBuffers[i], meshes.quad.indexCount, 1, 0, 0, 1);
				// Move viewport to display final composition in lower right corner
				viewport.x = viewport.width * 0.5f;
				viewport.y = viewport.height * 0.5f;
				vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);
			}

			// Final composition as full screen quad
			vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineList->get(enableSSAO ? "composition.ssao.enabled" : "composition.ssao.disabled"));
			vkCmdBindVertexBuffers(drawCmdBuffers[i], VERTEX_BUFFER_BIND_ID, 1, &meshes.quad.vertices.buf, offsets);
			vkCmdBindIndexBuffer(drawCmdBuffers[i], meshes.quad.indices.buf, 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexed(drawCmdBuffers[i], 6, 1, 0, 0, 1);

			vkCmdEndRenderPass(drawCmdBuffers[i]);

			VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
		}
	}

	void generateQuads()
	{
		// Setup vertices for multiple screen aligned quads
		// Used for displaying final result and debug 
		struct Vertex {
			float pos[3];
			float uv[2];
			float col[3];
			float normal[3];
			float tangent[3];
		};

		std::vector<Vertex> vertexBuffer;

		float x = 0.0f;
		float y = 0.0f;
		for (uint32_t i = 0; i < 3; i++)
		{
			// Last component of normal is used for debug display sampler index
			vertexBuffer.push_back({ { x + 1.0f, y + 1.0f, 0.0f },{ 1.0f, 1.0f },{ 1.0f, 1.0f, 1.0f },{ 0.0f, 0.0f, (float)i } });
			vertexBuffer.push_back({ { x,      y + 1.0f, 0.0f },{ 0.0f, 1.0f },{ 1.0f, 1.0f, 1.0f },{ 0.0f, 0.0f, (float)i } });
			vertexBuffer.push_back({ { x,      y,      0.0f },{ 0.0f, 0.0f },{ 1.0f, 1.0f, 1.0f },{ 0.0f, 0.0f, (float)i } });
			vertexBuffer.push_back({ { x + 1.0f, y,      0.0f },{ 1.0f, 0.0f },{ 1.0f, 1.0f, 1.0f },{ 0.0f, 0.0f, (float)i } });
			x += 1.0f;
			if (x > 1.0f)
			{
				x = 0.0f;
				y += 1.0f;
			}
		}

		createBuffer(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			vertexBuffer.size() * sizeof(Vertex),
			vertexBuffer.data(),
			&meshes.quad.vertices.buf,
			&meshes.quad.vertices.mem);

		// Setup indices
		std::vector<uint32_t> indexBuffer = { 0,1,2, 2,3,0 };
		for (uint32_t i = 0; i < 3; ++i)
		{
			uint32_t indices[6] = { 0,1,2, 2,3,0 };
			for (auto index : indices)
			{
				indexBuffer.push_back(i * 4 + index);
			}
		}
		meshes.quad.indexCount = indexBuffer.size();

		createBuffer(
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			indexBuffer.size() * sizeof(uint32_t),
			indexBuffer.data(),
			&meshes.quad.indices.buf,
			&meshes.quad.indices.mem);
	}

	void setupVertexDescriptions()
	{
		// Binding description
		vertices.bindingDescriptions.resize(1);
		vertices.bindingDescriptions[0] =
			vkTools::initializers::vertexInputBindingDescription(
				VERTEX_BUFFER_BIND_ID,
				sizeof(Vertex),
				VK_VERTEX_INPUT_RATE_VERTEX);

		// Attribute descriptions
		vertices.attributeDescriptions.resize(5);
		// Location 0: Position
		vertices.attributeDescriptions[0] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				0,
				VK_FORMAT_R32G32B32_SFLOAT,
				offsetof(Vertex, pos));
		// Location 1: Texture coordinates
		vertices.attributeDescriptions[1] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				1,
				VK_FORMAT_R32G32_SFLOAT,
				offsetof(Vertex, uv));
		// Location 2: Color
		vertices.attributeDescriptions[2] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				2,
				VK_FORMAT_R32G32B32_SFLOAT,
				offsetof(Vertex, color));
		// Location 3: Normal
		vertices.attributeDescriptions[3] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				3,
				VK_FORMAT_R32G32B32_SFLOAT,
				offsetof(Vertex, normal));
		// Location 4: Tangent
		vertices.attributeDescriptions[4] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				4,
				VK_FORMAT_R32G32B32_SFLOAT,
				offsetof(Vertex, tangent));

		vertices.inputState = vkTools::initializers::pipelineVertexInputStateCreateInfo();
		vertices.inputState.vertexBindingDescriptionCount = vertices.bindingDescriptions.size();
		vertices.inputState.pVertexBindingDescriptions = vertices.bindingDescriptions.data();
		vertices.inputState.vertexAttributeDescriptionCount = vertices.attributeDescriptions.size();
		vertices.inputState.pVertexAttributeDescriptions = vertices.attributeDescriptions.data();
	}

	void setupDescriptorPool()
	{
		std::vector<VkDescriptorPoolSize> poolSizes =
		{
			vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8),
			vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12)
		};

		VkDescriptorPoolCreateInfo descriptorPoolInfo =
			vkTools::initializers::descriptorPoolCreateInfo(
				poolSizes.size(),
				poolSizes.data(),
				4);

		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));
	}

	void setupLayoutsAndDescriptors()
	{
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;
		VkDescriptorSetLayoutCreateInfo setLayoutCreateInfo;
		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = vkTools::initializers::pipelineLayoutCreateInfo();
		VkDescriptorSetAllocateInfo descriptorAllocInfo = vkTools::initializers::descriptorSetAllocateInfo(descriptorPool, nullptr, 1);
		std::vector<VkWriteDescriptorSet> writeDescriptorSets;
		std::vector<VkDescriptorImageInfo> imageDescriptors;

		// Composition
		setLayoutBindings = {
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0),				// Vertex shader uniform buffer
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1),		// Position texture target / Scene colormap
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 2),		// Normals texture target
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 3),		// Albedo texture target
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 4),		// FS SSAO blurred
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 5),				// Fragment shader uniform buffer
		};
		setLayoutCreateInfo = vkTools::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings.data(), static_cast<uint32_t>(setLayoutBindings.size()));
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &setLayoutCreateInfo, nullptr, &descriptorSetLayouts.composition));
		pipelineLayoutCreateInfo.pSetLayouts = &descriptorSetLayouts.composition;
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.composition));
		descriptorAllocInfo.pSetLayouts = &descriptorSetLayouts.composition;
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorAllocInfo, &descriptorSets.composition));
		imageDescriptors = {
			vkTools::initializers::descriptorImageInfo(colorSampler, frameBuffers.offscreen.attachments[0].view, VK_IMAGE_LAYOUT_GENERAL),
			vkTools::initializers::descriptorImageInfo(colorSampler, frameBuffers.offscreen.attachments[1].view, VK_IMAGE_LAYOUT_GENERAL),
			vkTools::initializers::descriptorImageInfo(colorSampler, frameBuffers.offscreen.attachments[2].view, VK_IMAGE_LAYOUT_GENERAL),
			vkTools::initializers::descriptorImageInfo(colorSampler, frameBuffers.ssaoBlur.attachments[0].view, VK_IMAGE_LAYOUT_GENERAL),
		};	
		writeDescriptorSets = {
			vkTools::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &uniformBuffers.fullScreen.descriptor),	// Binding 0 : Vertex shader uniform buffer			
			vkTools::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, &imageDescriptors[0]),				// Binding 1 : Position texture target			
			vkTools::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, &imageDescriptors[1]),				// Binding 2 : Normals texture target			
			vkTools::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3, &imageDescriptors[2]),				// Binding 3 : Albedo texture target			
			vkTools::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, &imageDescriptors[3]),				// FS Sampler SSAO blurred
			vkTools::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 5, &uniformBuffers.sceneLights.descriptor),	// Binding 4 : Fragment shader uniform buffer
		};
		vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);

		// SSAO Generation
		setLayoutBindings = {
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0),						// FS Position+Depth
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1),						// FS Normals
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 2),						// FS SSAO Noise
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 3),								// FS SSAO Kernel UBO
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 4),								// FS Params UBO 
		};
		setLayoutCreateInfo = vkTools::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings.data(), static_cast<uint32_t>(setLayoutBindings.size()));
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &setLayoutCreateInfo, nullptr, &descriptorSetLayouts.ssao));
		pipelineLayoutCreateInfo.pSetLayouts = &descriptorSetLayouts.ssao;
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.ssao));
		descriptorAllocInfo.pSetLayouts = &descriptorSetLayouts.ssao;
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorAllocInfo, &descriptorSets.ssao));
		imageDescriptors = {
			vkTools::initializers::descriptorImageInfo(colorSampler, frameBuffers.offscreen.attachments[0].view, VK_IMAGE_LAYOUT_GENERAL),
			vkTools::initializers::descriptorImageInfo(colorSampler, frameBuffers.offscreen.attachments[1].view, VK_IMAGE_LAYOUT_GENERAL),
		};
		writeDescriptorSets = {
			vkTools::initializers::writeDescriptorSet(descriptorSets.ssao, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &imageDescriptors[0]),					// FS Position+Depth
			vkTools::initializers::writeDescriptorSet(descriptorSets.ssao, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, &imageDescriptors[1]),					// FS Normals
			vkTools::initializers::writeDescriptorSet(descriptorSets.ssao, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, &textures.ssaoNoise.descriptor),		// FS SSAO Noise
			vkTools::initializers::writeDescriptorSet(descriptorSets.ssao, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3, &uniformBuffers.ssaoKernel.descriptor),		// FS SSAO Kernel UBO
			vkTools::initializers::writeDescriptorSet(descriptorSets.ssao, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4, &uniformBuffers.ssaoParams.descriptor),		// FS SSAO Params UBO
		};
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, NULL);

		// SSAO Blur
		setLayoutBindings = {
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0),						// FS Sampler SSAO
		};
		setLayoutCreateInfo = vkTools::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings.data(), static_cast<uint32_t>(setLayoutBindings.size()));
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &setLayoutCreateInfo, nullptr, &descriptorSetLayouts.ssaoBlur));
		pipelineLayoutCreateInfo.pSetLayouts = &descriptorSetLayouts.ssaoBlur;
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.ssaoBlur));
		descriptorAllocInfo.pSetLayouts = &descriptorSetLayouts.ssaoBlur;
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorAllocInfo, &descriptorSets.ssaoBlur));
		imageDescriptors = {
			vkTools::initializers::descriptorImageInfo(colorSampler, frameBuffers.ssao.attachments[0].view, VK_IMAGE_LAYOUT_GENERAL),
		};
		writeDescriptorSets = {
			vkTools::initializers::writeDescriptorSet(descriptorSets.ssaoBlur, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &imageDescriptors[0]),
		};
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, NULL);

		// G-Buffer creation (offscreen scene rendering)
		setLayoutBindings = {
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0),				// Vertex shader uniform buffer
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1),		// Diffuse
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 2),		// Specular
			vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 3),		// Bump
		};
		setLayoutCreateInfo.pBindings = setLayoutBindings.data();
		setLayoutCreateInfo.bindingCount = setLayoutBindings.size();
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &setLayoutCreateInfo, nullptr, &descriptorSetLayouts.offscreen));
		pipelineLayoutCreateInfo.pSetLayouts = &descriptorSetLayouts.offscreen;
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.offscreen));
		descriptorAllocInfo.pSetLayouts = &descriptorSetLayouts.composition;
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorAllocInfo, &descriptorSets.offscreen));
		writeDescriptorSets = {
			vkTools::initializers::writeDescriptorSet(descriptorSets.offscreen, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &uniformBuffers.sceneMatrices.descriptor),// Binding 0 : Vertex shader uniform buffer			
			vkTools::initializers::writeDescriptorSet(descriptorSets.offscreen, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, &textures.colorMap.descriptor)	// Binding 1 : Scene color map
		};
		vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);
	}

	void preparePipelines()
	{
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState =
			vkTools::initializers::pipelineInputAssemblyStateCreateInfo(
				VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
				0,
				VK_FALSE);

		VkPipelineRasterizationStateCreateInfo rasterizationState =
			vkTools::initializers::pipelineRasterizationStateCreateInfo(
				VK_POLYGON_MODE_FILL,
				VK_CULL_MODE_BACK_BIT,
				VK_FRONT_FACE_CLOCKWISE,
				0);

		VkPipelineColorBlendAttachmentState blendAttachmentState =
			vkTools::initializers::pipelineColorBlendAttachmentState(
				0xf,
				VK_FALSE);

		VkPipelineColorBlendStateCreateInfo colorBlendState =
			vkTools::initializers::pipelineColorBlendStateCreateInfo(
				1,
				&blendAttachmentState);

		VkPipelineDepthStencilStateCreateInfo depthStencilState =
			vkTools::initializers::pipelineDepthStencilStateCreateInfo(
				VK_TRUE,
				VK_TRUE,
				VK_COMPARE_OP_LESS_OR_EQUAL);

		VkPipelineViewportStateCreateInfo viewportState =
			vkTools::initializers::pipelineViewportStateCreateInfo(1, 1, 0);

		VkPipelineMultisampleStateCreateInfo multisampleState =
			vkTools::initializers::pipelineMultisampleStateCreateInfo(
				VK_SAMPLE_COUNT_1_BIT,
				0);

		std::vector<VkDynamicState> dynamicStateEnables = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR
		};
		VkPipelineDynamicStateCreateInfo dynamicState =
			vkTools::initializers::pipelineDynamicStateCreateInfo(
				dynamicStateEnables.data(),
				dynamicStateEnables.size(),
				0);

		// Final composition pipeline
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

		VkGraphicsPipelineCreateInfo pipelineCreateInfo(vkTools::initializers::pipelineCreateInfo());

		pipelineCreateInfo.pVertexInputState = &vertices.inputState;
		pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
		pipelineCreateInfo.pRasterizationState = &rasterizationState;
		pipelineCreateInfo.pColorBlendState = &colorBlendState;
		pipelineCreateInfo.pMultisampleState = &multisampleState;
		pipelineCreateInfo.pViewportState = &viewportState;
		pipelineCreateInfo.pDepthStencilState = &depthStencilState;
		pipelineCreateInfo.pDynamicState = &dynamicState;
		pipelineCreateInfo.stageCount = shaderStages.size();
		pipelineCreateInfo.pStages = shaderStages.data();
		pipelineCreateInfo.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;

		/*
		VkPipelineRasterizationStateRasterizationOrderAMD rasterAMD = {};
		rasterAMD.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_RASTERIZATION_ORDER_AMD;
		rasterAMD.rasterizationOrder = VK_RASTERIZATION_ORDER_RELAXED_AMD;

		rasterizationState.pNext = &rasterAMD;
		*/

		// Final composition pipeline
		{
			pipelineCreateInfo.layout = pipelineLayouts.composition;
			pipelineCreateInfo.renderPass = renderPass;

			shaderStages[0] = loadShader(getAssetPath() + "shaders/composition.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
			shaderStages[1] = loadShader(getAssetPath() + "shaders/composition.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

			struct SpecializationData {
				int32_t enableSSAO = 1;
			} specializationData;

			std::vector<VkSpecializationMapEntry> specializationMapEntries;
			specializationMapEntries = {
				vkTools::initializers::specializationMapEntry(0, 0, sizeof(int32_t)),
			};
			VkSpecializationInfo specializationInfo = vkTools::initializers::specializationInfo(specializationMapEntries.size(), specializationMapEntries.data(), sizeof(specializationData), &specializationData);
			shaderStages[1].pSpecializationInfo = &specializationInfo;

			// SSAO enabled
			pipelineList->addGraphicsPipeline("composition.ssao.enabled", pipelineCreateInfo, pipelineCache);
			// SSAO disabled
			specializationData.enableSSAO = 0;
			pipelineList->addGraphicsPipeline("composition.ssao.disabled", pipelineCreateInfo, pipelineCache);
		}

		// Derivate info for other pipelines
		pipelineCreateInfo.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
		pipelineCreateInfo.basePipelineIndex = -1;
		pipelineCreateInfo.basePipelineHandle = pipelineList->get("composition.ssao.enabled");

		// Debug display pipeline
		shaderStages[0] = loadShader(getAssetPath() + "shaders/debug.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getAssetPath() + "shaders/debug.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		pipelineList->addGraphicsPipeline("debugdisplay", pipelineCreateInfo, pipelineCache);

		// Fill G-Buffer

		// Set constant parameters via specialization constants
		struct SpecializationData {
			float znear;
			float zfar;
			int32_t discard = 0;
		} specializationData;

		specializationData.znear = camera.znear;
		specializationData.zfar = camera.zfar;

		std::vector<VkSpecializationMapEntry> specializationMapEntries;
		specializationMapEntries = {
			vkTools::initializers::specializationMapEntry(0, offsetof(SpecializationData, znear), sizeof(float)),
			vkTools::initializers::specializationMapEntry(1, offsetof(SpecializationData, zfar), sizeof(float)),
			vkTools::initializers::specializationMapEntry(2, offsetof(SpecializationData, discard), sizeof(int32_t)),

		};
		VkSpecializationInfo specializationInfo = vkTools::initializers::specializationInfo(specializationMapEntries.size(), specializationMapEntries.data(), sizeof(specializationData), &specializationData);

		shaderStages[0] = loadShader(getAssetPath() + "shaders/mrt.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getAssetPath() + "shaders/mrt.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		shaderStages[1].pSpecializationInfo = &specializationInfo;

		pipelineCreateInfo.renderPass = frameBuffers.offscreen.renderPass;
		pipelineCreateInfo.layout = pipelineLayouts.offscreen;

		std::array<VkPipelineColorBlendAttachmentState, 3> blendAttachmentStates = {
			vkTools::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE),
			vkTools::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE),
			vkTools::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE)
		};

		colorBlendState.attachmentCount = blendAttachmentStates.size();
		colorBlendState.pAttachments = blendAttachmentStates.data();
		pipelineList->addGraphicsPipeline("scene.solid", pipelineCreateInfo, pipelineCache);

		// Transparent objects (discard by alpha)
		depthStencilState.depthWriteEnable = VK_FALSE;
		rasterizationState.cullMode = VK_CULL_MODE_NONE;
		specializationData.discard = 1;
		pipelineList->addGraphicsPipeline("scene.blend", pipelineCreateInfo, pipelineCache);

		// SSAO
		colorBlendState.attachmentCount = 1;

		VkPipelineVertexInputStateCreateInfo emptyInputState{};
		emptyInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		emptyInputState.vertexAttributeDescriptionCount = 0;
		emptyInputState.pVertexAttributeDescriptions = nullptr;
		emptyInputState.vertexBindingDescriptionCount = 0;
		emptyInputState.pVertexBindingDescriptions = nullptr;
		pipelineCreateInfo.pVertexInputState = &emptyInputState;

		pipelineCreateInfo.pVertexInputState = &emptyInputState;

		// SSAO Pass
		shaderStages[0] = loadShader(getAssetPath() + "shaders/fullscreen.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getAssetPath() + "shaders/ssao.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		{
			// Set constant parameters via specialization constants
			struct SpecializationData {
				uint32_t kernelSize = SSAO_KERNEL_SIZE;
				float radius = SSAO_RADIUS;
				float power = 1.5f;
			} specializationData;

			std::vector<VkSpecializationMapEntry> specializationMapEntries;
			specializationMapEntries = {
				vkTools::initializers::specializationMapEntry(0, offsetof(SpecializationData, kernelSize), sizeof(uint32_t)),	// SSAO Kernel size
				vkTools::initializers::specializationMapEntry(1, offsetof(SpecializationData, radius), sizeof(float)),			// SSAO radius
				vkTools::initializers::specializationMapEntry(2, offsetof(SpecializationData, power), sizeof(float)),			// SSAO power
			};

			VkSpecializationInfo specializationInfo = vkTools::initializers::specializationInfo(specializationMapEntries.size(), specializationMapEntries.data(), sizeof(specializationData), &specializationData);
			shaderStages[1].pSpecializationInfo = &specializationInfo;
			pipelineCreateInfo.renderPass = frameBuffers.ssao.renderPass;
			pipelineCreateInfo.layout = pipelineLayouts.ssao;
			pipelineList->addGraphicsPipeline("ssao.generate", pipelineCreateInfo, pipelineCache);
		}


		// SSAO blur pass
		shaderStages[0] = loadShader(getAssetPath() + "shaders/fullscreen.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getAssetPath() + "shaders/blur.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		pipelineCreateInfo.renderPass = frameBuffers.ssaoBlur.renderPass;
		pipelineCreateInfo.layout = pipelineLayouts.ssaoBlur;
		pipelineList->addGraphicsPipeline("ssao.blur", pipelineCreateInfo, pipelineCache);
	}

	inline float lerp(float a, float b, float f)
	{
		return a + f * (b - a);
	}

	// Prepare and initialize uniform buffer containing shader uniforms
	void prepareUniformBuffers()
	{
		// Fullscreen vertex shader
		vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&uniformBuffers.fullScreen,
			sizeof(uboVS));

		// Deferred vertex shader
		vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&uniformBuffers.sceneMatrices,
			sizeof(uboSceneMatrices));

		// Deferred fragment shader
		vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&uniformBuffers.sceneLights,
			sizeof(uboFragmentLights));

		setupLights();

		// Update
		updateUniformBuffersScreen();
		updateUniformBufferDeferredMatrices();
		updateUniformBufferDeferredLights();

		// SSAO

		// SSAO parameters 
		vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&uniformBuffers.ssaoParams,
			sizeof(uboSSAOParams));
		updateUniformBufferSSAOParams();

		std::uniform_real_distribution<float> rndDist(0.0f, 1.0f);
		std::random_device rndDev;
		std::default_random_engine rndGen;

		// Sample kernel
		std::vector<glm::vec4> ssaoKernel(SSAO_KERNEL_SIZE);
		for (uint32_t i = 0; i < SSAO_KERNEL_SIZE; ++i)
		{
			glm::vec3 sample(rndDist(rndGen) * 2.0 - 1.0, rndDist(rndGen) * 2.0 - 1.0, rndDist(rndGen));
			sample = glm::normalize(sample);
			sample *= rndDist(rndGen);
			float scale = float(i) / float(SSAO_KERNEL_SIZE);
			scale = lerp(0.1f, 1.0f, scale * scale);
			ssaoKernel[i] = glm::vec4(sample * scale, 0.0f);
		}

		// Upload as UBO
		vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&uniformBuffers.ssaoKernel,
			ssaoKernel.size() * sizeof(glm::vec4),
			ssaoKernel.data());

		// Random noise
		std::vector<glm::vec4> ssaoNoise(SSAO_NOISE_DIM * SSAO_NOISE_DIM);
		for (uint32_t i = 0; i < static_cast<uint32_t>(ssaoNoise.size()); i++)
		{
			ssaoNoise[i] = glm::vec4(rndDist(rndGen) * 2.0f - 1.0f, rndDist(rndGen) * 2.0f - 1.0f, 0.0f, 0.0f);
		}
		// Upload as texture
		textureLoader->createTexture(ssaoNoise.data(), ssaoNoise.size() * sizeof(glm::vec4), VK_FORMAT_R32G32B32A32_SFLOAT, SSAO_NOISE_DIM, SSAO_NOISE_DIM, &textures.ssaoNoise, VK_FILTER_NEAREST);
	}

	void updateUniformBuffersScreen()
	{
		if (debugDisplay)
		{
			uboVS.projection = glm::ortho(0.0f, 2.0f, 0.0f, 2.0f, -1.0f, 1.0f);
		}
		else
		{
			uboVS.projection = glm::ortho(0.0f, 1.0f, 0.0f, 1.0f, -1.0f, 1.0f);
		}
		uboVS.model = glm::mat4();

		VK_CHECK_RESULT(uniformBuffers.fullScreen.map());
		uniformBuffers.fullScreen.copyTo(&uboVS, sizeof(uboVS));
		uniformBuffers.fullScreen.unmap();
	}

	void updateUniformBufferDeferredMatrices()
	{
		uboSceneMatrices.projection = camera.matrices.perspective;
		uboSceneMatrices.view = camera.matrices.view;
		uboSceneMatrices.model = glm::mat4();

		uint8_t *pData;

		VK_CHECK_RESULT(uniformBuffers.sceneMatrices.map());
		uniformBuffers.sceneMatrices.copyTo(&uboSceneMatrices, sizeof(uboSceneMatrices));
		uniformBuffers.sceneMatrices.unmap();
	}

	void updateUniformBufferSSAOParams()
	{
		uboSSAOParams.projection = camera.matrices.perspective;

		VK_CHECK_RESULT(uniformBuffers.ssaoParams.map());
		uniformBuffers.ssaoParams.copyTo(&uboSSAOParams, sizeof(uboSSAOParams));
		uniformBuffers.ssaoParams.unmap();
	}

	float rnd(float range)
	{
		return range * (rand() / double(RAND_MAX));
	}

	void setupLight(Light *light, glm::vec3 pos, glm::vec3 color, float radius)
	{
		light->position = glm::vec4(pos, 1.0f);
		light->color = glm::vec4(color, 1.0f);
		light->radius = radius;
		// linear and quadratic falloff not used with new shader
	}

	// Initial light setup for the scene
	void setupLights()
	{	
		// 5 fixed lights
		std::array<glm::vec3, 5> lightColors;
		lightColors[0] = glm::vec3(1.0f, 0.0f, 0.0f);
		lightColors[1] = glm::vec3(1.0f, 0.7f, 0.7f);
		lightColors[2] = glm::vec3(1.0f, 0.0f, 0.0f);
		lightColors[3] = glm::vec3(0.0f, 0.0f, 1.0f);
		lightColors[4] = glm::vec3(1.0f, 0.0f, 0.0f);

		for (int32_t i = 0; i < lightColors.size(); i++)
		{
			setupLight(&uboFragmentLights.lights[i], glm::vec3((float)(i - 2.5f) * 50.0f, 10.0f, 0.0f), lightColors[i], 120.0f);
		}

		// Dynamic light moving over the floor
		setupLight(&uboFragmentLights.lights[0], { -sin(glm::radians(360.0f * timer)) * 120.0f , 2.5f, cos(glm::radians(360.0f * timer * 8.0f)) * 10.0f }, glm::vec3(1.0f), 100.0f);

		// Fire bowls
		setupLight(&uboFragmentLights.lights[5], { -48.75f, 16.0f, -17.8f }, { 1.0f, 0.6f, 0.0f }, 45.0f);
		setupLight(&uboFragmentLights.lights[6], { -48.75f, 16.0f,  18.4f }, { 1.0f, 0.6f, 0.0f }, 45.0f);
		// -62.5, 15, -18.5
		setupLight(&uboFragmentLights.lights[7], { 62.0f, 16.0f, -17.8f }, { 1.0f, 0.6f, 0.0f }, 45.0f);
		setupLight(&uboFragmentLights.lights[8], { 62.0f, 16.0f,  18.4f }, { 1.0f, 0.6f, 0.0f }, 45.0f);

		// 112.5 13.6 -42.8
		setupLight(&uboFragmentLights.lights[9], { 120.0f, 20.0f, -43.75f }, { 1.0f, 0.8f, 0.3f }, 75.0f);
		setupLight(&uboFragmentLights.lights[10], { 120.0f, 20.0f, 41.75f }, { 1.0f, 0.8f, 0.3f }, 75.0f);

		setupLight(&uboFragmentLights.lights[11], { -110.0f, 20.0f, -43.75f }, { 1.0f, 0.8f, 0.3f }, 75.0f);
		setupLight(&uboFragmentLights.lights[12], { -110.0f, 20.0f, 41.75f }, { 1.0f, 0.8f, 0.3f }, 75.0f);

		// Lion eyes
		setupLight(&uboFragmentLights.lights[13], { -122.0f, 18.0f, -3.2f }, { 1.0f, 0.3f, 0.3f }, 25.0f);
		setupLight(&uboFragmentLights.lights[14], { -122.0f, 18.0f,  3.2f }, { 0.3f, 1.0f, 0.3f }, 25.0f);

		setupLight(&uboFragmentLights.lights[15], { 135.0f, 18.0f, -3.2f }, { 0.3f, 0.3f, 1.0f }, 25.0f);
		setupLight(&uboFragmentLights.lights[16], { 135.0f, 18.0f,  3.2f }, { 1.0f, 1.0f, 0.3f }, 25.0f);

		for (int32_t i = 0; i < 17; i++)
		{
			//uboFragmentLights.lights[i].position *= 0.5f;
			uboFragmentLights.lights[i].position.y *= -1.0f;
		}

	}

	// Update fragment shader light positions for moving light sources
	void updateUniformBufferDeferredLights()
	{
		// Dynamic light
		if (attachLight)
		{
			// Attach to camera position
			uboFragmentLights.lights[0].position = glm::vec4(camera.position, 0.0f) * glm::vec4(-1.0f, -1.0f, -1.0f, 1.0f);
		}
		else
		{
			// Move across the floow
			uboFragmentLights.lights[0].position.x = -sin(glm::radians(360.0f * timer)) * 120.0f;
			uboFragmentLights.lights[0].position.z = cos(glm::radians(360.0f * timer * 8.0f)) * 10.0f;
		}

		// Fire bowls
		/*
		uboFragmentLights.lights[5].position.x += (2.5f * sin(glm::radians(360.0f * timer)));
		uboFragmentLights.lights[5].position.z += (2.5f * cos(glm::radians(360.0f * timer)));

		uboFragmentLights.lights[6].position.x += (2.5f * cos(glm::radians(360.0f * timer)));
		uboFragmentLights.lights[6].position.z += (2.5f * sin(glm::radians(360.0f * timer)));
		*/

		uboFragmentLights.viewPos = glm::vec4(camera.position, 0.0f) * glm::vec4(-1.0f);
		//uboFragmentLights.inv = glm::inverse(camera.matrices.view);
		uboFragmentLights.view = camera.matrices.view;
		uboFragmentLights.model = glm::mat4();

		VK_CHECK_RESULT(uniformBuffers.sceneLights.map());
		uniformBuffers.sceneLights.copyTo(&uboFragmentLights, sizeof(uboFragmentLights));
		uniformBuffers.sceneLights.unmap();
	}


	void loadScene()
	{
		VkCommandBuffer copyCmd = VulkanExampleBase::createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
		scene = new Scene(device, queue, textureLoader, &uniformBuffers.sceneMatrices);

#if defined(__ANDROID__)
		scene->assetManager = androidApp->activity->assetManager;
#endif
		scene->assetPath = getAssetPath();

		scene->load(getAssetPath() + "sponza.dae", copyCmd);
		vkFreeCommandBuffers(device, cmdPool, 1, &copyCmd);
	}

	void draw()
	{
		VulkanExampleBase::prepareFrame();

		// Offscreen rendering

		// Wait for swap chain presentation to finish
		submitInfo.pWaitSemaphores = &semaphores.presentComplete;
		// Signal ready with offscreen semaphore
		submitInfo.pSignalSemaphores = &offscreenSemaphore;

		// Submit work
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &offScreenCmdBuffer;
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));

		// Scene rendering
		// Wait for offscreen semaphore
		submitInfo.pWaitSemaphores = &offscreenSemaphore;
		// Signal ready with render complete semaphpre
		submitInfo.pSignalSemaphores = &semaphores.renderComplete;
		// Submit work
		submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));

		VulkanExampleBase::submitFrame();
	}

	void prepare()
	{
		VulkanExampleBase::prepare();

		// todo : sep func
		deviceMemProps = deviceMemoryProperties;

		loadTextures();
		generateQuads();
		setupVertexDescriptions();
		prepareOffscreenFramebuffers();
		prepareUniformBuffers();
		setupDescriptorPool();
		setupLayoutsAndDescriptors();
		preparePipelines();
		loadScene();
		buildCommandBuffers();
		buildDeferredCommandBuffer();
		prepared = true;
	}

	virtual void render()
	{
		if (!prepared)
			return;
		draw();

		if (!paused)
		{
			updateUniformBufferDeferredLights();
		}
	}

	virtual void viewChanged()
	{
		updateUniformBufferDeferredMatrices();
		updateUniformBufferSSAOParams();
		updateTextOverlay();
	}

	void toggleDebugDisplay()
	{
		debugDisplay = !debugDisplay;
		reBuildCommandBuffers();
		updateUniformBuffersScreen();
	}

	void toggleSSAO()
	{
		enableSSAO = !enableSSAO;
		reBuildCommandBuffers();
		buildDeferredCommandBuffer(true);
	}

	virtual void keyPressed(uint32_t keyCode)
	{
		switch (keyCode)
		{
		case KEY_F1:
		case GAMEPAD_BUTTON_A:
			toggleDebugDisplay();
			updateTextOverlay();
			break;
		case KEY_F2:
			toggleSSAO();
			break;
		case KEY_L:
		case GAMEPAD_BUTTON_B:
			attachLight = !attachLight;
			break;
		}
	}

	virtual void getOverlayText(VulkanTextOverlay *textOverlay)
	{
#if defined(__ANDROID__)
		textOverlay->addText("Press \"Button A\" to toggle render targets", 5.0f, 85.0f, VulkanTextOverlay::alignLeft);
#else
		//textOverlay->addText("Press \"1\" to toggle render targets", 5.0f, 85.0f, VulkanTextOverlay::alignLeft);
#endif
		// Render targets
		if (debugDisplay)
		{
			textOverlay->addText("World Position", (float)width * 0.25f, (float)height * 0.5f - 25.0f, VulkanTextOverlay::alignCenter);
			textOverlay->addText("World normals", (float)width * 0.75f, (float)height * 0.5f - 25.0f, VulkanTextOverlay::alignCenter);
			textOverlay->addText("Color", (float)width * 0.25f, (float)height - 25.0f, VulkanTextOverlay::alignCenter);
			textOverlay->addText("Final image", (float)width * 0.75f, (float)height - 25.0f, VulkanTextOverlay::alignCenter);
		}
	}
};

VULKAN_EXAMPLE_MAIN()