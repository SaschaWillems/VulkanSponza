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

// Texture properties
#define TEX_DIM 1024
#define TEX_FILTER VK_FILTER_LINEAR

// Offscreen frame buffer properties
#define FB_DIM TEX_DIM

// Vertex layout for this example
std::vector<vkMeshLoader::VertexLayout> vertexLayout =
{
	vkMeshLoader::VERTEX_LAYOUT_POSITION,
	vkMeshLoader::VERTEX_LAYOUT_UV,
	vkMeshLoader::VERTEX_LAYOUT_COLOR,
	vkMeshLoader::VERTEX_LAYOUT_NORMAL
};

struct Vertex
{
	glm::vec3 pos;
	glm::vec2 uv;
	glm::vec3 color;
	glm::vec3 normal;
	// todo : tangents
};

struct {
	VkPipeline deferred;
	VkPipeline debug;
	struct {
		VkPipeline solid; // todo : rename
		//VkPipeline bump;
		VkPipeline blend;
	} scene;
} pipelines;

struct SceneMaterial 
{
	std::string name;
	vkTools::VulkanTexture diffuse;
	vkTools::VulkanTexture specular;
	vkTools::VulkanTexture bump;
	bool hasAlpha = false;
	bool hasBump = false;
	bool hasSpecular = false;
	VkPipeline *pipeline;
};

struct SceneMesh
{
	VkBuffer vertexBuffer;
	VkDeviceMemory vertexMemory;

	VkBuffer indexBuffer;
	VkDeviceMemory indexMemory;

	uint32_t indexCount;

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
	
	// todo 
	vkTools::UniformData *defaultUBO;
	
	VkDescriptorPool descriptorPool;

	vkTools::VulkanTextureLoader *textureLoader;

	const aiScene* aScene;

	void loadMaterials()
	{
		materials.resize(aScene->mNumMaterials);
		
//		LOGD("Material count %d", materials.size());		

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
			// Diffuse
			aScene->mMaterials[i]->GetTexture(aiTextureType_DIFFUSE, 0, &texturefile);
			if (aScene->mMaterials[i]->GetTextureCount(aiTextureType_DIFFUSE) > 0)
			{
				std::cout << "  Diffuse: \"" << texturefile.C_Str() << "\"" << std::endl;
				std::string fileName = std::string(texturefile.C_Str());
				std::replace(fileName.begin(), fileName.end(), '\\', '/');
#if defined(__ANDROID__)
				LOGD("Diffuse texture %s from %s", texturefile.C_Str(), assetPath.c_str());
#endif
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
			}
			// Bump (map_bump is mapped to height by assimp)
			if (aScene->mMaterials[i]->GetTextureCount(aiTextureType_HEIGHT) > 0)
			{
				aScene->mMaterials[i]->GetTexture(aiTextureType_HEIGHT, 0, &texturefile);
				std::cout << "  Bump: \"" << texturefile.C_Str() << "\"" << std::endl;
				std::string fileName = std::string(texturefile.C_Str());
				std::replace(fileName.begin(), fileName.end(), '\\', '/');
				textureLoader->loadTexture(assetPath + fileName, VK_FORMAT_BC2_UNORM_BLOCK, &materials[i].bump);
				materials[i].hasBump = true;
			}
			else
			{
				std::cout << "  Material has no bump, using dummy texture!" << std::endl;
				textureLoader->loadTexture(assetPath + "sponza/dummy.dds", VK_FORMAT_BC2_UNORM_BLOCK, &materials[i].bump);
			}
			// Mask
			if (aScene->mMaterials[i]->GetTextureCount(aiTextureType_OPACITY) > 0)
			{
				aScene->mMaterials[i]->GetTexture(aiTextureType_OPACITY, 0, &texturefile);
				std::cout << "  Opacity: \"" << texturefile.C_Str() << "\"" << std::endl;
				materials[i].hasAlpha = true;
			}

			materials[i].pipeline = &pipelines.scene.solid;

			if (materials[i].hasBump)
			{
//				materials[i].pipeline = &pipelines.scene.bump;
			}
		}

	}

	void loadMeshes(VkCommandBuffer copyCmd)		
	{
		meshes.resize(aScene->mNumMeshes);
		for (uint32_t i = 0; i < meshes.size(); i++)
		{
			aiMesh *aMesh = aScene->mMeshes[i];

			std::cout << "Mesh \"" << aMesh->mName.C_Str() << "\"" << std::endl;
			std::cout << "	Material: \"" << materials[aMesh->mMaterialIndex].name << "\"" << std::endl;
			std::cout << "	Faces: " << aMesh->mNumFaces << std::endl;
			
			meshes[i].material = &materials[aMesh->mMaterialIndex];

			// Vertices
			std::vector<Vertex> vertices;			
			vertices.resize(aMesh->mNumVertices);

			bool hasUV = aMesh->HasTextureCoords(0);

			for (uint32_t i = 0; i < aMesh->mNumVertices; i++)
			{
				vertices[i].pos = glm::make_vec3(&aMesh->mVertices[i].x);
				vertices[i].pos.y = -vertices[i].pos.y;
				if (hasUV)
				{
					vertices[i].uv = glm::make_vec2(&aMesh->mTextureCoords[0][i].x);
				}
				vertices[i].normal = glm::make_vec3(&aMesh->mNormals[i].x);
				vertices[i].color = glm::vec3(1.0f); // todo : take from material
				// todo : tangents
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

		// Generate descriptor sets for all meshes
		// todo : think about a nicer solution, better suited per material?

		// Decriptor pool
		std::vector<VkDescriptorPoolSize> poolSizes;
		poolSizes.push_back(vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, meshes.size()));
		poolSizes.push_back(vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, meshes.size() * 2));

		VkDescriptorPoolCreateInfo descriptorPoolInfo =
			vkTools::initializers::descriptorPoolCreateInfo(
				poolSizes.size(),
				poolSizes.data(),
				meshes.size());

		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));

		// Shared descriptor set layout
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;
		// Binding 0 : UBO
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_SHADER_STAGE_VERTEX_BIT,
			0));
		// Binding 1 : Diffuse
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			1));
		// Binding 2 : Bump
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			2));

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

			std::vector<VkDescriptorImageInfo> texDescriptors;
			texDescriptors.push_back(vkTools::initializers::descriptorImageInfo(
				meshes[i].material->diffuse.sampler,
				meshes[i].material->diffuse.view,
				VK_IMAGE_LAYOUT_GENERAL));
			texDescriptors.push_back(vkTools::initializers::descriptorImageInfo(
				meshes[i].material->bump.sampler,
				meshes[i].material->bump.view,
				VK_IMAGE_LAYOUT_GENERAL));

			// todo : additional maps

			std::vector<VkWriteDescriptorSet> writeDescriptorSets;

			// Binding 0 : Vertex shader uniform buffer
			writeDescriptorSets.push_back(
				vkTools::initializers::writeDescriptorSet(
					meshes[i].descriptorSet,
					VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					0,
					&defaultUBO->descriptor));
			// Image bindings
			for (uint32_t j = 0; j < texDescriptors.size(); j++)
			{
				writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
					meshes[i].descriptorSet,
					VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					1 + j,
					&texDescriptors[j]));
			}

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

	// Same for all meshes in the scene
	VkDescriptorSetLayout descriptorSetLayout;
	VkPipelineLayout pipelineLayout;

	Scene(VkDevice device, VkQueue queue, vkTools::VulkanTextureLoader *textureloader, vkTools::UniformData *defaultUBO)
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

		int flags = aiProcess_FlipWindingOrder | aiProcess_PreTransformVertices | aiProcess_CalcTangentSpace;// | aiProcess_GenSmoothNormals;

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

	struct {
		vkTools::VulkanTexture colorMap;
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
	} uboVS, uboOffscreenVS;

	struct Light {
		glm::vec4 position;
		glm::vec4 color;
		float radius;
		float quadraticFalloff;
		float linearFalloff;
		float _pad;
	};

	struct {
		Light lights[13];
		glm::vec4 viewPos;
	} uboFragmentLights;

	struct {
		vkTools::UniformData vsFullScreen;
		vkTools::UniformData vsOffscreen;
		vkTools::UniformData fsLights;
	} uniformData;

	struct {
		VkPipelineLayout deferred;
		VkPipelineLayout offscreen;
	} pipelineLayouts;

	struct {
		VkDescriptorSet deferred;
		VkDescriptorSet offscreen;
	} descriptorSets;

	struct {
		VkDescriptorSetLayout deferred;
		VkDescriptorSetLayout offscreen;
	} descriptorSetLayouts;

	// Framebuffer for offscreen rendering
	struct FrameBufferAttachment {
		VkImage image;
		VkDeviceMemory mem;
		VkImageView view;
		VkFormat format;
	};
	struct FrameBuffer {
		int32_t width, height;
		VkFramebuffer frameBuffer;
		FrameBufferAttachment position, normal, albedo;
		FrameBufferAttachment depth;
		VkRenderPass renderPass;

	} offScreenFrameBuf;

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
		camera.setPerspective(60.0f, (float)width / (float)height, 0.1f, 512.0f);
		camera.setRotation(glm::vec3(0.0f, -90.0f, 0.0f));
		camera.setTranslation(glm::vec3(0.0f, 10.0f, 0.0f));

		camera.movementSpeed = 20.0f * 2.0f;

		timerSpeed = 0.075f;
		rotationSpeed = 0.15f;
#if defined(_WIN32)
		setupConsole("VulkanExample");
#endif
		srand(time(NULL));
//		paused = true;
	}

	~VulkanExample()
	{
		// Clean up used Vulkan resources 
		// Note : Inherited destructor cleans up resources stored in base class

		vkDestroySampler(device, colorSampler, nullptr);

		// Frame buffer

		// Color attachments
		vkDestroyImageView(device, offScreenFrameBuf.position.view, nullptr);
		vkDestroyImage(device, offScreenFrameBuf.position.image, nullptr);
		vkFreeMemory(device, offScreenFrameBuf.position.mem, nullptr);

		vkDestroyImageView(device, offScreenFrameBuf.normal.view, nullptr);
		vkDestroyImage(device, offScreenFrameBuf.normal.image, nullptr);
		vkFreeMemory(device, offScreenFrameBuf.normal.mem, nullptr);

		vkDestroyImageView(device, offScreenFrameBuf.albedo.view, nullptr);
		vkDestroyImage(device, offScreenFrameBuf.albedo.image, nullptr);
		vkFreeMemory(device, offScreenFrameBuf.albedo.mem, nullptr);

		// Depth attachment
		vkDestroyImageView(device, offScreenFrameBuf.depth.view, nullptr);
		vkDestroyImage(device, offScreenFrameBuf.depth.image, nullptr);
		vkFreeMemory(device, offScreenFrameBuf.depth.mem, nullptr);

		vkDestroyFramebuffer(device, offScreenFrameBuf.frameBuffer, nullptr);

		vkDestroyPipeline(device, pipelines.deferred, nullptr);
		vkDestroyPipeline(device, pipelines.scene.solid, nullptr);
		//vkDestroyPipeline(device, pipelines.scene.bump, nullptr);
		vkDestroyPipeline(device, pipelines.scene.blend, nullptr);
		vkDestroyPipeline(device, pipelines.debug, nullptr);

		vkDestroyPipelineLayout(device, pipelineLayouts.deferred, nullptr);
		vkDestroyPipelineLayout(device, pipelineLayouts.offscreen, nullptr);

		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.deferred, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.offscreen, nullptr);

		// Meshes
		vkMeshLoader::freeMeshBufferResources(device, &meshes.quad);

		// Uniform buffers
		vkTools::destroyUniformData(device, &uniformData.vsOffscreen);
		vkTools::destroyUniformData(device, &uniformData.vsFullScreen);
		vkTools::destroyUniformData(device, &uniformData.fsLights);

		vkFreeCommandBuffers(device, cmdPool, 1, &offScreenCmdBuffer);

		vkDestroyRenderPass(device, offScreenFrameBuf.renderPass, nullptr);

		textureLoader->destroyTexture(textures.colorMap);

		vkDestroySemaphore(device, offscreenSemaphore, nullptr);

		delete(scene);
	}

	// Create a frame buffer attachment
	void createAttachment(
		VkFormat format,
		VkImageUsageFlagBits usage,
		FrameBufferAttachment *attachment,
		VkCommandBuffer layoutCmd)
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
		image.extent.width = offScreenFrameBuf.width;
		image.extent.height = offScreenFrameBuf.height;
		image.extent.depth = 1;
		image.mipLevels = 1;
		image.arrayLayers = 1;
		image.samples = VK_SAMPLE_COUNT_1_BIT;
		image.tiling = VK_IMAGE_TILING_OPTIMAL;
		image.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT;

		VkMemoryAllocateInfo memAlloc = vkTools::initializers::memoryAllocateInfo();
		VkMemoryRequirements memReqs;

		VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &attachment->image));
		vkGetImageMemoryRequirements(device, attachment->image, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = getMemTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &attachment->mem));
		VK_CHECK_RESULT(vkBindImageMemory(device, attachment->image, attachment->mem, 0));

		if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
		{
			// Set the initial layout to shader read instead of attachment 
			// This is done as the render loop does the actualy image layout transitions
			vkTools::setImageLayout(
				layoutCmd,
				attachment->image,
				aspectMask,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		else
		{
			vkTools::setImageLayout(
				layoutCmd,
				attachment->image,
				aspectMask,
				VK_IMAGE_LAYOUT_UNDEFINED,
				imageLayout);
		}

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
	void prepareOffscreenFramebuffer()
	{
		VkCommandBuffer layoutCmd = VulkanExampleBase::createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);

		// todo : supersampling?
		offScreenFrameBuf.width = width;
		offScreenFrameBuf.height = height;

		// Color attachments

		// (World space) Positions
		createAttachment(
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			&offScreenFrameBuf.position,
			layoutCmd);

		// (World space) Normals
		createAttachment(
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			&offScreenFrameBuf.normal,
			layoutCmd);

		// Albedo (color)
		createAttachment(
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			&offScreenFrameBuf.albedo,
			layoutCmd);

		// Depth attachment

		// Find a suitable depth format
		VkFormat attDepthFormat;
		VkBool32 validDepthFormat = vkTools::getSupportedDepthFormat(physicalDevice, &attDepthFormat);
		assert(validDepthFormat);

		createAttachment(
			attDepthFormat,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			&offScreenFrameBuf.depth,
			layoutCmd);

		VulkanExampleBase::flushCommandBuffer(layoutCmd, queue, true);

		// Set up separate renderpass with references
		// to the color and depth attachments

		std::array<VkAttachmentDescription, 4> attachmentDescs = {};

		// Init attachment properties
		for (uint32_t i = 0; i < 4; ++i)
		{
			attachmentDescs[i].samples = VK_SAMPLE_COUNT_1_BIT;
			attachmentDescs[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachmentDescs[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachmentDescs[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachmentDescs[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			if (i == 3)
			{
				attachmentDescs[i].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				attachmentDescs[i].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			}
			else
			{
				attachmentDescs[i].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				attachmentDescs[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			}
		}

		// Formats
		attachmentDescs[0].format = offScreenFrameBuf.position.format;
		attachmentDescs[1].format = offScreenFrameBuf.normal.format;
		attachmentDescs[2].format = offScreenFrameBuf.albedo.format;
		attachmentDescs[3].format = offScreenFrameBuf.depth.format;

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
		subpass.colorAttachmentCount = colorReferences.size();
		subpass.pDepthStencilAttachment = &depthReference;

		VkRenderPassCreateInfo renderPassInfo = {};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.pAttachments = attachmentDescs.data();
		renderPassInfo.attachmentCount = attachmentDescs.size();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;

		VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassInfo, nullptr, &offScreenFrameBuf.renderPass));

		std::array<VkImageView, 4> attachments;
		attachments[0] = offScreenFrameBuf.position.view;
		attachments[1] = offScreenFrameBuf.normal.view;
		attachments[2] = offScreenFrameBuf.albedo.view;
		// depth
		attachments[3] = offScreenFrameBuf.depth.view;

		VkFramebufferCreateInfo fbufCreateInfo = {};
		fbufCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbufCreateInfo.pNext = NULL;
		fbufCreateInfo.renderPass = offScreenFrameBuf.renderPass;
		fbufCreateInfo.pAttachments = attachments.data();
		fbufCreateInfo.attachmentCount = attachments.size();
		fbufCreateInfo.width = offScreenFrameBuf.width;
		fbufCreateInfo.height = offScreenFrameBuf.height;
		fbufCreateInfo.layers = 1;

		VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &offScreenFrameBuf.frameBuffer));
		// Create sampler to sample from the color attachments
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
	void buildDeferredCommandBuffer()
	{
		if (offScreenCmdBuffer == VK_NULL_HANDLE)
		{
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
		renderPassBeginInfo.renderPass = offScreenFrameBuf.renderPass;
		renderPassBeginInfo.framebuffer = offScreenFrameBuf.frameBuffer;
		renderPassBeginInfo.renderArea.extent.width = offScreenFrameBuf.width;
		renderPassBeginInfo.renderArea.extent.height = offScreenFrameBuf.height;
		renderPassBeginInfo.clearValueCount = clearValues.size();
		renderPassBeginInfo.pClearValues = clearValues.data();

		VK_CHECK_RESULT(vkBeginCommandBuffer(offScreenCmdBuffer, &cmdBufInfo));

		std::vector<FrameBufferAttachment> attachments = { offScreenFrameBuf.position, offScreenFrameBuf.normal, offScreenFrameBuf.albedo };

		// Change back layout of the color attachments after sampling in the fragment shader
		for (auto attachment : attachments)
		{
			vkTools::setImageLayout(
				offScreenCmdBuffer,
				attachment.image,
				VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		}

		vkCmdBeginRenderPass(offScreenCmdBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport = vkTools::initializers::viewport(
			(float)offScreenFrameBuf.width,
			(float)offScreenFrameBuf.height,
			0.0f,
			1.0f);
		vkCmdSetViewport(offScreenCmdBuffer, 0, 1, &viewport);

		VkRect2D scissor = vkTools::initializers::rect2D(
			offScreenFrameBuf.width,
			offScreenFrameBuf.height,
			0,
			0);
		vkCmdSetScissor(offScreenCmdBuffer, 0, 1, &scissor);

		vkCmdBindPipeline(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.scene.solid);

		VkDeviceSize offsets[1] = { 0 };

		for (auto mesh : scene->meshes)
		{
			if (mesh.material->hasAlpha)
			{
				continue;
			}
			// todo : perf
			vkCmdBindPipeline(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, *mesh.material->pipeline);
			vkCmdBindDescriptorSets(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelineLayout, 0, 1, &mesh.descriptorSet, 0, NULL);
			vkCmdBindVertexBuffers(offScreenCmdBuffer, VERTEX_BUFFER_BIND_ID, 1, &mesh.vertexBuffer, offsets);
			vkCmdBindIndexBuffer(offScreenCmdBuffer, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexed(offScreenCmdBuffer, mesh.indexCount, 1, 0, 0, 0);
		}

		vkCmdBindPipeline(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.scene.blend);

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

		vkCmdEndRenderPass(offScreenCmdBuffer);

		// Change back layout of the color attachments after sampling in the fragment shader
		for (auto attachment : attachments)
		{
			vkTools::setImageLayout(
				offScreenCmdBuffer,
				attachment.image,
				VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
			renderPassBeginInfo.framebuffer = frameBuffers[i];

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
			vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.deferred, 0, 1, &descriptorSets.deferred, 0, NULL);

			if (debugDisplay)
			{
				vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.debug);
				vkCmdBindVertexBuffers(drawCmdBuffers[i], VERTEX_BUFFER_BIND_ID, 1, &meshes.quad.vertices.buf, offsets);
				vkCmdBindIndexBuffer(drawCmdBuffers[i], meshes.quad.indices.buf, 0, VK_INDEX_TYPE_UINT32);
				vkCmdDrawIndexed(drawCmdBuffers[i], meshes.quad.indexCount, 1, 0, 0, 1);
				// Move viewport to display final composition in lower right corner
				viewport.x = viewport.width * 0.5f;
				viewport.y = viewport.height * 0.5f;
				vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);
			}

			// Final composition as full screen quad
			vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.deferred);
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
				vkMeshLoader::vertexSize(vertexLayout),
				VK_VERTEX_INPUT_RATE_VERTEX);

		// Attribute descriptions
		vertices.attributeDescriptions.resize(4);
		// Location 0 : Position
		vertices.attributeDescriptions[0] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				0,
				VK_FORMAT_R32G32B32_SFLOAT,
				0);
		// Location 1 : Texture coordinates
		vertices.attributeDescriptions[1] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				1,
				VK_FORMAT_R32G32_SFLOAT,
				sizeof(float) * 3);
		// Location 2 : Color
		vertices.attributeDescriptions[2] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				2,
				VK_FORMAT_R32G32B32_SFLOAT,
				sizeof(float) * 5);
		// Location 3 : Normal
		vertices.attributeDescriptions[3] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				3,
				VK_FORMAT_R32G32B32_SFLOAT,
				sizeof(float) * 8);

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
			vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8)
		};

		VkDescriptorPoolCreateInfo descriptorPoolInfo =
			vkTools::initializers::descriptorPoolCreateInfo(
				poolSizes.size(),
				poolSizes.data(),
				2);

		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));
	}

	void setupDescriptorSetLayout()
	{
		// Deferred shading layout
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;

		// Binding 0 : Vertex shader uniform buffer
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_SHADER_STAGE_VERTEX_BIT,
			0));
		// Binding 1 : Position texture target / Scene colormap
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			1));
		// Binding 2 : Normals texture target
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			2));
		// Binding 3 : Albedo texture target
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			3));
		// Binding 4 : Fragment shader uniform buffer
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			4));

		VkDescriptorSetLayoutCreateInfo descriptorLayout =
			vkTools::initializers::descriptorSetLayoutCreateInfo(
				setLayoutBindings.data(),
				setLayoutBindings.size());

		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayouts.deferred));

		VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo =
			vkTools::initializers::pipelineLayoutCreateInfo(
				&descriptorSetLayouts.deferred,
				1);

		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pPipelineLayoutCreateInfo, nullptr, &pipelineLayouts.deferred));

		// Offscreen (scene) rendering pipeline layout
		setLayoutBindings.clear();
		// Binding 0 : Vertex shader uniform buffer
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_SHADER_STAGE_VERTEX_BIT,
			0));
		// Binding 1 : Diffuse
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			1));
		// Binding 1 : Bump
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			2));

		descriptorLayout.pBindings = setLayoutBindings.data();
		descriptorLayout.bindingCount = setLayoutBindings.size();

		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayouts.offscreen));

		pPipelineLayoutCreateInfo.pSetLayouts = &descriptorSetLayouts.offscreen;

		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pPipelineLayoutCreateInfo, nullptr, &pipelineLayouts.offscreen));
	}

	void setupDescriptorSet()
	{
		// Textured quad descriptor set
		VkDescriptorSetAllocateInfo allocInfo =
			vkTools::initializers::descriptorSetAllocateInfo(
				descriptorPool,
				&descriptorSetLayouts.deferred,
				1);

		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSets.deferred));

		// Image descriptor for the offscreen texture targets
		VkDescriptorImageInfo texDescriptorPosition =
			vkTools::initializers::descriptorImageInfo(
				colorSampler,
				offScreenFrameBuf.position.view,
				VK_IMAGE_LAYOUT_GENERAL);

		VkDescriptorImageInfo texDescriptorNormal =
			vkTools::initializers::descriptorImageInfo(
				colorSampler,
				offScreenFrameBuf.normal.view,
				VK_IMAGE_LAYOUT_GENERAL);

		VkDescriptorImageInfo texDescriptorAlbedo =
			vkTools::initializers::descriptorImageInfo(
				colorSampler,
				offScreenFrameBuf.albedo.view,
				VK_IMAGE_LAYOUT_GENERAL);

		std::vector<VkWriteDescriptorSet> writeDescriptorSets =
		{
			// Binding 0 : Vertex shader uniform buffer
			vkTools::initializers::writeDescriptorSet(
				descriptorSets.deferred,
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				0,
				&uniformData.vsFullScreen.descriptor),
			// Binding 1 : Position texture target
			vkTools::initializers::writeDescriptorSet(
				descriptorSets.deferred,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				1,
				&texDescriptorPosition),
			// Binding 2 : Normals texture target
			vkTools::initializers::writeDescriptorSet(
				descriptorSets.deferred,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				2,
				&texDescriptorNormal),
			// Binding 3 : Albedo texture target
			vkTools::initializers::writeDescriptorSet(
				descriptorSets.deferred,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				3,
				&texDescriptorAlbedo),
			// Binding 4 : Fragment shader uniform buffer
			vkTools::initializers::writeDescriptorSet(
				descriptorSets.deferred,
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				4,
				&uniformData.fsLights.descriptor),
		};

		vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);

		// Offscreen (scene)

		allocInfo.pSetLayouts = &descriptorSetLayouts.deferred;

		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSets.offscreen));

		VkDescriptorImageInfo texDescriptorSceneColormap =
			vkTools::initializers::descriptorImageInfo(
				textures.colorMap.sampler,
				textures.colorMap.view,
				VK_IMAGE_LAYOUT_GENERAL);

		std::vector<VkWriteDescriptorSet> offScreenWriteDescriptorSets =
		{
			// Binding 0 : Vertex shader uniform buffer
			vkTools::initializers::writeDescriptorSet(
				descriptorSets.offscreen,
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				0,
				&uniformData.vsOffscreen.descriptor),
			// Binding 1 : Scene color map
			vkTools::initializers::writeDescriptorSet(
				descriptorSets.offscreen,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				1,
				&texDescriptorSceneColormap)
		};
		vkUpdateDescriptorSets(device, offScreenWriteDescriptorSets.size(), offScreenWriteDescriptorSets.data(), 0, NULL);
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

		// Final fullscreen pass pipeline
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

		shaderStages[0] = loadShader(getAssetPath() + "shaders/deferred.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getAssetPath() + "shaders/deferred.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

		VkGraphicsPipelineCreateInfo pipelineCreateInfo =
			vkTools::initializers::pipelineCreateInfo(
				pipelineLayouts.deferred,
				renderPass,
				0);

		/*
		VkPipelineRasterizationStateRasterizationOrderAMD rasterAMD = {};
		rasterAMD.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_RASTERIZATION_ORDER_AMD;
		rasterAMD.rasterizationOrder = VK_RASTERIZATION_ORDER_RELAXED_AMD;

		rasterizationState.pNext = &rasterAMD;
		*/

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

		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.deferred));

		// Derivate info for other pipelines
		pipelineCreateInfo.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
		pipelineCreateInfo.basePipelineIndex = -1;
		pipelineCreateInfo.basePipelineHandle = pipelines.deferred;

		// Debug display pipeline

		shaderStages[0] = loadShader(getAssetPath() + "shaders/debug.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getAssetPath() + "shaders/debug.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.debug));

		// Offscreen pipelines

		shaderStages[0] = loadShader(getAssetPath() + "shaders/mrt.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getAssetPath() + "shaders/mrt.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

		// Separate render pass
		pipelineCreateInfo.renderPass = offScreenFrameBuf.renderPass;

		// Separate layout
		pipelineCreateInfo.layout = pipelineLayouts.offscreen;

		// Solid

		std::array<VkPipelineColorBlendAttachmentState, 3> blendAttachmentStates = {
			vkTools::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE),
			vkTools::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE),
			vkTools::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE)
		};

		colorBlendState.attachmentCount = blendAttachmentStates.size();
		colorBlendState.pAttachments = blendAttachmentStates.data();

		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.scene.solid));

		//// Bump
		//shaderStages[1] = loadShader(getAssetPath() + "shaders/mrt_bump.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

		//VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.scene.bump));

		// Alpha blending (no depth writes)

		//rasterAMD.rasterizationOrder = VK_RASTERIZATION_ORDER_STRICT_AMD;


		shaderStages[1] = loadShader(getAssetPath() + "shaders/mrt_discard.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

		depthStencilState.depthWriteEnable = VK_FALSE;
		rasterizationState.cullMode = VK_CULL_MODE_NONE;

		for (uint32_t i = 0; i < blendAttachmentStates.size(); i++)
		{
			blendAttachmentStates[i].blendEnable = VK_TRUE;
			blendAttachmentStates[i].colorBlendOp = VK_BLEND_OP_ADD;
			blendAttachmentStates[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			blendAttachmentStates[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			blendAttachmentStates[i].alphaBlendOp = VK_BLEND_OP_ADD;
			blendAttachmentStates[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			blendAttachmentStates[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		}

		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.scene.blend));

	}

	// Prepare and initialize uniform buffer containing shader uniforms
	void prepareUniformBuffers()
	{
		// Fullscreen vertex shader
		createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			sizeof(uboVS),
			&uboVS,
			&uniformData.vsFullScreen.buffer,
			&uniformData.vsFullScreen.memory,
			&uniformData.vsFullScreen.descriptor);

		// Deferred vertex shader
		createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			sizeof(uboOffscreenVS),
			&uboOffscreenVS,
			&uniformData.vsOffscreen.buffer,
			&uniformData.vsOffscreen.memory,
			&uniformData.vsOffscreen.descriptor);

		// Deferred fragment shader
		createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			sizeof(uboFragmentLights),
			&uboFragmentLights,
			&uniformData.fsLights.buffer,
			&uniformData.fsLights.memory,
			&uniformData.fsLights.descriptor);

		setupLights();

		// Update
		updateUniformBuffersScreen();
		updateUniformBufferDeferredMatrices();
		updateUniformBufferDeferredLights();
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

		uint8_t *pData;
		VK_CHECK_RESULT(vkMapMemory(device, uniformData.vsFullScreen.memory, 0, sizeof(uboVS), 0, (void **)&pData));
		memcpy(pData, &uboVS, sizeof(uboVS));
		vkUnmapMemory(device, uniformData.vsFullScreen.memory);
	}

	void updateUniformBufferDeferredMatrices()
	{
		uboOffscreenVS.projection = camera.matrices.perspective;
		uboOffscreenVS.view = camera.matrices.view;
		uboOffscreenVS.model = glm::mat4();

		uint8_t *pData;
		VK_CHECK_RESULT(vkMapMemory(device, uniformData.vsOffscreen.memory, 0, sizeof(uboOffscreenVS), 0, (void **)&pData));
		memcpy(pData, &uboOffscreenVS, sizeof(uboOffscreenVS));
		vkUnmapMemory(device, uniformData.vsOffscreen.memory);
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
		lightColors[1] = glm::vec3(1.0f);
		lightColors[2] = glm::vec3(1.0f, 0.0f, 0.0f);
		lightColors[3] = glm::vec3(0.0f, 0.0f, 1.0f);
		lightColors[4] = glm::vec3(1.0f, 0.0f, 0.0f);

		for (int32_t i = 0; i < lightColors.size(); i++)
		{
			setupLight(&uboFragmentLights.lights[i], glm::vec3((float)(i - 2.5f) * 50.0f, 10.0f, 0.0f), lightColors[i], 100.0f);
		}

		// Dynamic light moving over the floor
		setupLight(&uboFragmentLights.lights[0], { -sin(glm::radians(360.0f * timer)) * 120.0f , 2.5f, cos(glm::radians(360.0f * timer * 8.0f)) * 10.0f }, glm::vec3(1.0f), 100.0f);

		// Fire bowls
		setupLight(&uboFragmentLights.lights[5], { -48.75f, 14.0f, -17.8f }, { 1.0f, 0.6f, 0.0f }, 15.0f);
		setupLight(&uboFragmentLights.lights[6], { -48.75f, 14.0f,  18.4f }, { 1.0f, 0.6f, 0.0f }, 15.0f);
		// -62.5, 15, -18.5
		setupLight(&uboFragmentLights.lights[7], { 62.0f, 14.0f, -17.8f }, { 1.0f, 0.6f, 0.0f }, 15.0f);
		setupLight(&uboFragmentLights.lights[8], { 62.0f, 14.0f,  18.4f }, { 1.0f, 0.6f, 0.0f }, 15.0f);

		// 112.5 13.6 -42.8
		setupLight(&uboFragmentLights.lights[9], { 120.0f, 20.0f, -43.75f }, { 1.0f, 0.8f, 0.3f }, 75.0f);
		setupLight(&uboFragmentLights.lights[10], { 120.0f, 20.0f, 41.75f }, { 1.0f, 0.8f, 0.3f }, 75.0f);

		setupLight(&uboFragmentLights.lights[11], { -110.0f, 20.0f, -43.75f }, { 1.0f, 0.8f, 0.3f }, 75.0f);
		setupLight(&uboFragmentLights.lights[12], { -110.0f, 20.0f, 41.75f }, { 1.0f, 0.8f, 0.3f }, 75.0f);
	}

	// Update fragment shader light positions for moving light sources
	void updateUniformBufferDeferredLights()
	{
		// Dynamic light moving over the floor
		uboFragmentLights.lights[0].position.x = -sin(glm::radians(360.0f * timer)) * 120.0f;
		uboFragmentLights.lights[0].position.z = cos(glm::radians(360.0f * timer * 8.0f)) * 10.0f;

		// Fire bowls
		/*
		uboFragmentLights.lights[5].position.x += (2.5f * sin(glm::radians(360.0f * timer)));
		uboFragmentLights.lights[5].position.z += (2.5f * cos(glm::radians(360.0f * timer)));

		uboFragmentLights.lights[6].position.x += (2.5f * cos(glm::radians(360.0f * timer)));
		uboFragmentLights.lights[6].position.z += (2.5f * sin(glm::radians(360.0f * timer)));
		*/

		uboFragmentLights.viewPos = glm::vec4(-cameraPos, 0.0f);

		// todo: map persistent
		uint8_t *pData;
		VK_CHECK_RESULT(vkMapMemory(device, uniformData.fsLights.memory, 0, VK_WHOLE_SIZE, 0, (void **)&pData));
		memcpy(pData, &uboFragmentLights, sizeof(uboFragmentLights));
		vkUnmapMemory(device, uniformData.fsLights.memory);
	}


	void loadScene()
	{
		VkCommandBuffer copyCmd = VulkanExampleBase::createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
		scene = new Scene(device, queue, textureLoader, &uniformData.vsOffscreen);

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
		prepareOffscreenFramebuffer();
		prepareUniformBuffers();
		setupDescriptorSetLayout();
		preparePipelines();
		setupDescriptorPool();
		setupDescriptorSet();
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
		updateTextOverlay();
	}

	void toggleDebugDisplay()
	{
		debugDisplay = !debugDisplay;
		reBuildCommandBuffers();
		updateUniformBuffersScreen();
	}

	virtual void keyPressed(uint32_t keyCode)
	{
		switch (keyCode)
		{
		case 0x31:
		case GAMEPAD_BUTTON_A:
			toggleDebugDisplay();
			updateTextOverlay();
			break;
		}
	}

	virtual void getOverlayText(VulkanTextOverlay *textOverlay)
	{
#if defined(__ANDROID__)
		textOverlay->addText("Press \"Button A\" to toggle render targets", 5.0f, 85.0f, VulkanTextOverlay::alignLeft);
#else
		textOverlay->addText("Press \"1\" to toggle render targets", 5.0f, 85.0f, VulkanTextOverlay::alignLeft);
#endif
		std::stringstream ss;
		ss << camera;
		textOverlay->addText(ss.str(), 5.0, 105.0f, VulkanTextOverlay::alignLeft);
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