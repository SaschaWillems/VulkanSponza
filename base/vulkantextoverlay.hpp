/*
* Text overlay class for displaying debug information
*
* Copyright (C) 2016 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <vector>
#include <sstream>
#include <iomanip>

#include <vulkan/vulkan.h>
#include "vulkantools.h"

#include "stb/stb_font_consolas_24_usascii.inl"

namespace vkTools
{

#define STB_FONT_WIDTH STB_FONT_consolas_24_usascii_BITMAP_WIDTH
#define STB_FONT_HEIGHT STB_FONT_consolas_24_usascii_BITMAP_HEIGHT 
#define STB_FIRST_CHAR STB_FONT_consolas_24_usascii_FIRST_CHAR
#define STB_NUM_CHARS STB_FONT_consolas_24_usascii_NUM_CHARS

	class VulkanTextOverlay
	{
	private:
		VkPhysicalDevice physicalDevice;
		VkDevice device;
		VkPhysicalDeviceMemoryProperties deviceMemoryProperties;
		VkFormat colorFormat;
		VkFormat depthFormat;
		VkExtent2D windowSize;

		VkSampler sampler;
		VkImage image;
		VkImageView view;
		VkBuffer buffer;
//		VkDeviceMemory memory;
		VkDeviceMemory imageMemory;
		VkDescriptorPool descriptorPool;
		VkDescriptorSetLayout descriptorSetLayout;
		VkDescriptorSet descriptorSet;
		VkPipelineLayout pipelineLayout;
		VkPipelineCache pipelineCache;
		VkPipeline pipeline;
		VkCommandPool commandPool;
		std::vector<VkCommandBuffer> cmdBuffers;
		std::vector<VkFramebuffer> frameBuffers;

		VkRenderPass renderPass;

		// Try to find appropriate memory type for a memory allocation
		VkBool32 getMemoryType(uint32_t typeBits, VkFlags properties, uint32_t *typeIndex)
		{
			for (int i = 0; i < 32; i++) {
				if ((typeBits & 1) == 1) {
					if ((deviceMemoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
					{
						*typeIndex = i;
						return true;
					}
				}
				typeBits >>= 1;
			}
			return false;
		}

		// todo : duplicate code, android
		VkPipelineShaderStageCreateInfo loadShader(std::string fileName, VkShaderStageFlagBits stage)
		{
			VkPipelineShaderStageCreateInfo shaderStage = {};
			shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStage.stage = stage;
#if defined(__ANDROID__)
			shaderStage.module = vkTools::loadShader(androidApp->activity->assetManager, fileName.c_str(), device, stage);
#else
			shaderStage.module = vkTools::loadShader(fileName.c_str(), device, stage);
#endif
			shaderStage.pName = "main"; // todo : make param
//			assert(shaderStage.module != NULL);
			//shaderModules.push_back(shaderStage.module);
			return shaderStage;
		}

	public:

		// todo : move to private!
		stb_fontchar stbFontData[STB_FONT_consolas_24_usascii_NUM_CHARS];
		uint32_t numLetters;
		VkDeviceMemory memory;


		VulkanTextOverlay::VulkanTextOverlay(
			VkPhysicalDevice physicalDevice, 
			VkDevice device, 
			std::vector<VkFramebuffer> framebuffers,
			VkFormat colorformat,
			VkFormat depthformat,
			VkExtent2D windowsize)
		{
			this->physicalDevice = physicalDevice;
			this->device = device;
			this->colorFormat = colorformat;
			this->depthFormat = depthformat;
			this->windowSize = windowsize;
			this->frameBuffers = framebuffers;

			vkGetPhysicalDeviceMemoryProperties(physicalDevice, &deviceMemoryProperties);
			cmdBuffers.resize(framebuffers.size());
			prepareResources();
			prepareRenderPass();
			preparePipeline();
		}

		// Prepare all vulkan resources required to render the font
		// The text overlay uses separate resources for descriptors (pool, sets, layouts), pipelines and command buffers
		void prepareResources()
		{
			static unsigned char font24pixels[STB_FONT_HEIGHT][STB_FONT_WIDTH];
			stb_font_consolas_24_usascii(stbFontData, font24pixels, STB_FONT_consolas_24_usascii_BITMAP_HEIGHT);

			VkDeviceSize bufferSize = 1024 * sizeof(glm::vec4);

			VkBufferCreateInfo bufferInfo = vkTools::initializers::bufferCreateInfo(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, bufferSize);
			vkTools::checkResult(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));

			VkMemoryRequirements memReqs;
			VkMemoryAllocateInfo allocInfo = vkTools::initializers::memoryAllocateInfo();

			vkGetBufferMemoryRequirements(device, buffer, &memReqs);
			allocInfo.allocationSize = memReqs.size;
			getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &allocInfo.memoryTypeIndex);

			vkTools::checkResult(vkAllocateMemory(device, &allocInfo, nullptr, &memory));
			vkTools::checkResult(vkBindBufferMemory(device, buffer, memory, 0));

			VkImageCreateInfo imageInfo = vkTools::initializers::imageCreateInfo();
			imageInfo.imageType = VK_IMAGE_TYPE_2D;
			imageInfo.format = VK_FORMAT_R8_UNORM;
			imageInfo.extent.width = STB_FONT_WIDTH;
			imageInfo.extent.height = STB_FONT_HEIGHT;
			imageInfo.extent.depth = 1;
			imageInfo.mipLevels = 1;
			imageInfo.arrayLayers = 1;
			imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
			imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
			imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			imageInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

			vkTools::checkResult(vkCreateImage(device, &imageInfo, nullptr, &image));

			allocInfo.allocationSize = STB_FONT_WIDTH * 128;

			vkTools::checkResult(vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory));
			vkTools::checkResult(vkBindImageMemory(device, image, imageMemory, 0));

			VkImageViewCreateInfo imageViewInfo = vkTools::initializers::imageViewCreateInfo();
			imageViewInfo.image = image;
			imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			imageViewInfo.format = imageInfo.format;
			imageViewInfo.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,	VK_COMPONENT_SWIZZLE_A };
			imageViewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

			vkTools::checkResult(vkCreateImageView(device, &imageViewInfo, nullptr, &view));

			uint8_t *data;
			vkTools::checkResult(vkMapMemory(device, imageMemory, 0, allocInfo.allocationSize, 0, (void **)&data));
			memcpy(data, &font24pixels[0][0], STB_FONT_WIDTH * STB_FONT_HEIGHT);
			vkUnmapMemory(device, imageMemory);

			// Sampler
			VkSamplerCreateInfo samplerInfo = vkTools::initializers::samplerCreateInfo();
			samplerInfo.magFilter = VK_FILTER_LINEAR;
			samplerInfo.minFilter = VK_FILTER_LINEAR;
			samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			samplerInfo.mipLodBias = 0.0f;
			samplerInfo.compareOp = VK_COMPARE_OP_NEVER;
			samplerInfo.minLod = 0.0f;
			samplerInfo.maxLod = 1.0f;
			samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
			vkTools::checkResult(vkCreateSampler(device, &samplerInfo, nullptr, &sampler));

			// Descriptor
			// Font uses a separate descriptor pool
			std::array<VkDescriptorPoolSize, 1> poolSizes;
			poolSizes[0] = vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1);

			VkDescriptorPoolCreateInfo descriptorPoolInfo =
				vkTools::initializers::descriptorPoolCreateInfo(
					poolSizes.size(),
					poolSizes.data(),
					1);

			vkTools::checkResult(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));

			// Descriptor set layout
			std::array<VkDescriptorSetLayoutBinding, 1> setLayoutBindings;
			setLayoutBindings[0] = vkTools::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0);

			VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo =
				vkTools::initializers::descriptorSetLayoutCreateInfo(
					setLayoutBindings.data(),
					setLayoutBindings.size());

			vkTools::checkResult(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutInfo, nullptr, &descriptorSetLayout));

			// Pipeline layout
			VkPipelineLayoutCreateInfo pipelineLayoutInfo =
				vkTools::initializers::pipelineLayoutCreateInfo(
					&descriptorSetLayout,
					1);

			vkTools::checkResult(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout));

			// Descriptor set
			VkDescriptorSetAllocateInfo descriptorSetAllocInfo =
				vkTools::initializers::descriptorSetAllocateInfo(
					descriptorPool,
					&descriptorSetLayout,
					1);

			vkTools::checkResult(vkAllocateDescriptorSets(device, &descriptorSetAllocInfo, &descriptorSet));

			VkDescriptorImageInfo texDescriptor =
				vkTools::initializers::descriptorImageInfo(
					sampler,
					view,
					VK_IMAGE_LAYOUT_GENERAL);

			std::array<VkWriteDescriptorSet, 1> writeDescriptorSets;
			writeDescriptorSets[0] = vkTools::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &texDescriptor);
			vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);

			// Command buffer

			// Pool
			VkCommandPoolCreateInfo cmdPoolInfo = {};
			cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			cmdPoolInfo.queueFamilyIndex = 0; // todo : pass from example base / swap chain
			cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			vkTools::checkResult(vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &commandPool));

			VkCommandBufferAllocateInfo cmdBufAllocateInfo =
				vkTools::initializers::commandBufferAllocateInfo(
					commandPool,
					VK_COMMAND_BUFFER_LEVEL_PRIMARY,
					(uint32_t)cmdBuffers.size());

			vkTools::checkResult(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, cmdBuffers.data()));

			// Pipeline cache
			VkPipelineCacheCreateInfo pipelineCacheCreateInfo = {};
			pipelineCacheCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
			vkTools::checkResult(vkCreatePipelineCache(device, &pipelineCacheCreateInfo, nullptr, &pipelineCache));
		}

		// Prepare a separate pipeline for the font rendering decoupled from the main application
		void preparePipeline()
		{
			VkPipelineInputAssemblyStateCreateInfo inputAssemblyState =
				vkTools::initializers::pipelineInputAssemblyStateCreateInfo(
					VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
					0,
					VK_FALSE);

			VkPipelineRasterizationStateCreateInfo rasterizationState =
				vkTools::initializers::pipelineRasterizationStateCreateInfo(
					VK_POLYGON_MODE_FILL,
					VK_CULL_MODE_NONE,
					VK_FRONT_FACE_CLOCKWISE,
					0);

			VkPipelineColorBlendAttachmentState blendAttachmentState =
				vkTools::initializers::pipelineColorBlendAttachmentState(0xf, VK_TRUE);

			blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
			blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
			blendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
			blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			blendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;

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

			// todo : blending

			std::array<VkVertexInputBindingDescription, 2> vertexBindings = {};
			vertexBindings[0] = vkTools::initializers::vertexInputBindingDescription(0, sizeof(glm::vec4), VK_VERTEX_INPUT_RATE_VERTEX);
			vertexBindings[1] = vkTools::initializers::vertexInputBindingDescription(1, sizeof(glm::vec4), VK_VERTEX_INPUT_RATE_VERTEX);

			std::array<VkVertexInputAttributeDescription, 2> vertexAttribs = {};
			vertexAttribs[0] = vkTools::initializers::vertexInputAttributeDescription(0, 0, VK_FORMAT_R32G32_SFLOAT, 0);
			vertexAttribs[1] = vkTools::initializers::vertexInputAttributeDescription(1, 1, VK_FORMAT_R32G32_SFLOAT, sizeof(glm::vec2));

			VkPipelineVertexInputStateCreateInfo inputState = vkTools::initializers::pipelineVertexInputStateCreateInfo();
			inputState.vertexBindingDescriptionCount = vertexBindings.size();
			inputState.pVertexBindingDescriptions = vertexBindings.data();
			inputState.vertexAttributeDescriptionCount = vertexAttribs.size();
			inputState.pVertexAttributeDescriptions = vertexAttribs.data();

			std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

			//shaderStages[0] = loadShader(getAssetPath() + "shaders/base/font.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
			//shaderStages[1] = loadShader(getAssetPath() + "shaders/base/font.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
			shaderStages[0] = loadShader("../data/shaders/base/font.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
			shaderStages[1] = loadShader("../data/shaders/base/font.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

			VkGraphicsPipelineCreateInfo pipelineCreateInfo =
				vkTools::initializers::pipelineCreateInfo(
					pipelineLayout,
					renderPass,
					0);

			pipelineCreateInfo.pVertexInputState = &inputState;
			pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
			pipelineCreateInfo.pRasterizationState = &rasterizationState;
			pipelineCreateInfo.pColorBlendState = &colorBlendState;
			pipelineCreateInfo.pMultisampleState = &multisampleState;
			pipelineCreateInfo.pViewportState = &viewportState;
			pipelineCreateInfo.pDepthStencilState = &depthStencilState;
			pipelineCreateInfo.pDynamicState = &dynamicState;
			pipelineCreateInfo.stageCount = shaderStages.size();
			pipelineCreateInfo.pStages = shaderStages.data();

			vkTools::checkResult(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipeline));
		}

		// Prepare a separate render pass for rendering the text as an overlay
		void prepareRenderPass()
		{
			VkAttachmentDescription attachments[2] = {};

			// Color attachment
			attachments[0].format = colorFormat;
			attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
			attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			// Depth attachment
			attachments[1].format = depthFormat;
			attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
			attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentReference colorReference = {};
			colorReference.attachment = 0;
			colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentReference depthReference = {};
			depthReference.attachment = 1;
			depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkSubpassDescription subpass = {};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.flags = 0;
			subpass.inputAttachmentCount = 0;
			subpass.pInputAttachments = NULL;
			subpass.colorAttachmentCount = 1;
			subpass.pColorAttachments = &colorReference;
			subpass.pResolveAttachments = NULL;
			subpass.pDepthStencilAttachment = &depthReference;
			subpass.preserveAttachmentCount = 0;
			subpass.pPreserveAttachments = NULL;

			VkRenderPassCreateInfo renderPassInfo = {};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.pNext = NULL;
			renderPassInfo.attachmentCount = 2;
			renderPassInfo.pAttachments = attachments;
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = 0;
			renderPassInfo.pDependencies = NULL;

			vkTools::checkResult(vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass));
		}

		// Add text to the current buffer
		// todo : ripped from intel stardust example
		int addText(glm::vec4 **ptr, const char *str, int x, int y)
		{
			// todo : scale as property
			float recip_width = 0.75f / (float)windowSize.width;
			float recip_height = 0.75f / (float)windowSize.height;
			int letters = 0;

			while (*str)
			{
				int char_codepoint = *str++;
				stb_fontchar *cd = &stbFontData[char_codepoint - STB_FIRST_CHAR];

				const float ydir = -1.0;

				(*ptr)->x = (x + cd->x0) * 2.0f * recip_width - 1.0f;
				(*ptr)->y = ydir * (2.0f - (y + cd->y0) * 2.0f * recip_height - 1.0f);
				(*ptr)->z = cd->s0;
				(*ptr)->w = cd->t0;
				(*ptr)++;

				(*ptr)->x = (x + cd->x1) * 2.0f * recip_width - 1.0f;
				(*ptr)->y = ydir * (2.0f - (y + cd->y0) * 2.0f * recip_height - 1.0f);
				(*ptr)->z = cd->s1;
				(*ptr)->w = cd->t0;
				(*ptr)++;

				(*ptr)->x = (x + cd->x0) * 2.0f * recip_width - 1.0f;
				(*ptr)->y = ydir * (2.0f - (y + cd->y1) * 2.0f * recip_height - 1.0f);
				(*ptr)->z = cd->s0;
				(*ptr)->w = cd->t1;
				(*ptr)++;

				(*ptr)->x = (x + cd->x1) * 2.0f * recip_width - 1.0f;
				(*ptr)->y = ydir * (2.0f - (y + cd->y1) * 2.0f * recip_height - 1.0f);
				(*ptr)->z = cd->s1;
				(*ptr)->w = cd->t1;
				(*ptr)++;

				x += cd->advance_int;
				letters++;
			}

			return letters;
		}

		// Needs to be called by the application
		void updateCommandBuffers()
		{
			VkCommandBufferBeginInfo cmdBufInfo = vkTools::initializers::commandBufferBeginInfo();

			VkClearValue clearValues[1];
			clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };

			VkRenderPassBeginInfo renderPassBeginInfo = vkTools::initializers::renderPassBeginInfo();
			renderPassBeginInfo.renderPass = renderPass;
			renderPassBeginInfo.renderArea.extent = windowSize;
			renderPassBeginInfo.clearValueCount = 1;
			renderPassBeginInfo.pClearValues = clearValues;

			for (int32_t i = 0; i < cmdBuffers.size(); ++i)
			{
				renderPassBeginInfo.framebuffer = frameBuffers[i];

				vkTools::checkResult(vkBeginCommandBuffer(cmdBuffers[i], &cmdBufInfo));

				vkCmdBeginRenderPass(cmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

				VkViewport viewport = vkTools::initializers::viewport((float)windowSize.width, (float)windowSize.height, 0.0f, 1.0f);
				vkCmdSetViewport(cmdBuffers[i], 0, 1, &viewport);

				VkRect2D scissor = vkTools::initializers::rect2D(windowSize.width, windowSize.height, 0, 0);
				vkCmdSetScissor(cmdBuffers[i], 0, 1, &scissor);

				vkCmdBindPipeline(cmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				vkCmdBindDescriptorSets(cmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, NULL);

				VkDeviceSize offsets = 0;
				vkCmdBindVertexBuffers(cmdBuffers[i], 0, 1, &buffer, &offsets);
				vkCmdBindVertexBuffers(cmdBuffers[i], 1, 1, &buffer, &offsets);
				for (uint32_t j = 0; j < numLetters; j++)
				{
					vkCmdDraw(cmdBuffers[i], 4, 1, j * 4, 0);
				}

				vkCmdEndRenderPass(cmdBuffers[i]);

				vkTools::checkResult(vkEndCommandBuffer(cmdBuffers[i]));
			}
		}

		// Submit the text command buffers to a queue
		// Does a queue wait idle
		void submit(VkQueue queue, uint32_t bufferindex)
		{
			VkSubmitInfo submitInfo = {};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submitInfo.commandBufferCount = 1;
			submitInfo.pCommandBuffers = &cmdBuffers[bufferindex];

			vkTools::checkResult(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
			//vkTools::checkResult(vkQueueWaitIdle(queue));
		}

	};
}