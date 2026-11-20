#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <variant>
#include <stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include "CloudPass.hpp"
#include "../../ShaderLibrary.hpp"
#include "../../../VulkanInit.hpp"
#include "../../../../Config.hpp"
#include "../../../../ApplicationDeps.hpp"
import Volcano;
#include "../../../../Logger.hpp"
#include "../../../../resources/ImageLoad.hpp"

namespace Volcano::Engine::Passes {


void CloudPass::Create(const ShaderLibrary& shaders)
{
    if (!scene.LoadCells()) return;

    try
    {
        for (Slot& slot : slots)
        {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = sizeof(CloudScene::Vertex) * CloudScene::MAX_VERTICES;
            bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &slot.buffer, &slot.allocation, &info) != VK_SUCCESS)
            {
                slot.buffer = VK_NULL_HANDLE;
                throw std::runtime_error("[ERROR] Failed to create cloud vertex buffer.");
            }
            slot.mapped = static_cast<CloudScene::Vertex*>(info.pMappedData);
        }
        CreatePipeline(shaders);
    }
    catch (...)
    {
        Destroy();
        throw;
    }
    Log::Info("Cloud pass active.");
}

void CloudPass::ReloadShaders(const ShaderLibrary& shaders)
{
    if (!scene.Loaded()) return;
    DestroyPipeline();
    CreatePipeline(shaders);
}

void CloudPass::ReloadTextures(const ShaderLibrary& shaders)
{
    if (!scene.Loaded())
    {
        // The first load failed, so nothing was created; try the whole pass again.
        Destroy();
        Create(shaders);
        return;
    }

    if (!scene.LoadCells()) return;
    for (Slot& slot : slots) slot.mode = -1; // Force every mesh to rebuild from the new cells.
}

void CloudPass::Record(FrameContext& frame)
{
    if (pipeline == VK_NULL_HANDLE) return;

    // Where the grid is, its style, color and transform: shared with the Direct3D 11 pass.
    CloudScene::Draw draw;
    if (!scene.Plan(*frame.scene, frame.state->config, draw)) return;

    Slot& slot = slots[frame.frameIndex % FRAME_SLOTS];
    if (slot.mode != draw.mode || slot.centerX != draw.centerX || slot.centerZ != draw.centerZ)
    {
        slot.vertexCount = scene.BuildMesh(slot.mapped, draw.centerX, draw.centerZ, draw.mode);
        slot.centerX = draw.centerX;
        slot.centerZ = draw.centerZ;
        slot.mode = draw.mode;
    }
    if (slot.vertexCount == 0) return;

    VkCommandBuffer cmd = frame.commandBuffer;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &slot.buffer, &vertexOffset);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        sizeof(CloudScene::Push), &draw.push);
    vkCmdDraw(cmd, slot.vertexCount, 1, 0, 0);
}

void CloudPass::CreatePipeline(const ShaderLibrary& shaders)
{
    VkDevice dev = GetDevice();
    VkShaderModule vertModule = CreateShaderModule(shaders, VERTEX_SHADER);
    VkShaderModule fragModule = VK_NULL_HANDLE;
    try
    {
        fragModule = CreateShaderModule(shaders, FRAGMENT_SHADER);
    }
    catch (...)
    {
        vkDestroyShaderModule(dev, vertModule, nullptr);
        throw;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{ 0, sizeof(CloudScene::Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[2]{};
    attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CloudScene::Vertex, x) };
    attributes[1] = { 1, 0, VK_FORMAT_R32_SFLOAT, offsetof(CloudScene::Vertex, shade) };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Behind terrain and entities, but never in front of them in the depth
    // buffer the Hi-Z pyramid is built from.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CloudScene::Push) };
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    VkResult result = vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout);
    if (result == VK_SUCCESS)
    {
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        result = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        if (result != VK_SUCCESS) pipeline = VK_NULL_HANDLE;
    }
    else
    {
        pipelineLayout = VK_NULL_HANDLE;
    }

    vkDestroyShaderModule(dev, fragModule, nullptr);
    vkDestroyShaderModule(dev, vertModule, nullptr);

    if (result != VK_SUCCESS)
    {
        DestroyPipeline();
        throw std::runtime_error("[ERROR] Failed to create cloud pipeline.");
    }
}

void CloudPass::DestroyPipeline()
{
    VkDevice dev = GetDevice();
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
}

void CloudPass::Destroy()
{
    DestroyPipeline();
    for (Slot& slot : slots)
    {
        if (slot.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, slot.buffer, slot.allocation);
        slot = Slot{};
    }
}

} // namespace Volcano::Engine::Passes
