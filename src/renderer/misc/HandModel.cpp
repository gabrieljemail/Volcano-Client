#include "HandModel.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "ItemRenderer.hpp"
#include "HandScene.hpp"
#include "../VulkanInit.hpp"
#include "../entity/EntityRenderer.hpp"
#include "../entity/models/EntityJsonModel.hpp"
#include "../../ApplicationDeps.hpp"
import Volcano;
#include "../../Logger.hpp"
#include "../../Math.hpp"
#include "../../Player.hpp"
#include "../terrain/models/World.hpp"
#include "../../TickLoop.hpp"
#include "../../interaction/InteractionManager.hpp"
#include "../../inventory/ItemBehavior.hpp"
#include "../../inventory/ItemRegistry.hpp"

namespace Volcano {

namespace {

struct ArmPush {
    glm::mat4 mvp;
    glm::vec4 shade; // x: brightness.
};

} // namespace

void HandModel::Create(const Engine::ShaderLibrary& shaders)
{
    // Wide (Steve) arms: vanilla's box + part pivot, in model units. The left
    // arm has its own spot on the 64x64 skin sheet.
    BuildArm(rightArmMesh, HandScene::RIGHT_ARM_MIN, HandScene::RIGHT_ARM_UV);
    BuildArm(leftArmMesh, HandScene::LEFT_ARM_MIN, HandScene::LEFT_ARM_UV);
    CreatePipeline(shaders);
    ready = true;
}

void HandModel::Destroy()
{
    if (!ready && pipeline == VK_NULL_HANDLE && rightArmMesh.vertices.buffer == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(GetDevice());
    DestroyPipeline();
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(GetDevice(), pipelineLayout, nullptr);
    pipelineLayout = VK_NULL_HANDLE;
    for (ArmMesh* mesh : { &rightArmMesh, &leftArmMesh })
    {
        DestroyBuffer(mesh->vertices);
        DestroyBuffer(mesh->indices);
        mesh->indexCount = 0;
    }
    ready = false;
}

void HandModel::DestroyPipeline()
{
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
}

void HandModel::DestroyBuffer(GpuBuffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, buffer.buffer, buffer.allocation);
    buffer = GpuBuffer{};
}

HandModel::GpuBuffer HandModel::Upload(const void* data, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    GpuBuffer result;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &info) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] HandModel: failed to allocate an arm buffer.");
    }
    std::memcpy(info.pMappedData, data, static_cast<size_t>(size));
    vmaFlushAllocation(vmaAllocator, result.allocation, 0, VK_WHOLE_SIZE);
    return result;
}

void HandModel::BuildArm(ArmMesh& mesh, const glm::vec3& boxMin, const glm::vec2& uv)
{
    std::vector<EntityJsonModel::Vertex> vertices;
    std::vector<uint32_t> indices;
    HandScene::BuildArmGeometry(boxMin, uv, vertices, indices);

    mesh.vertices = Upload(vertices.data(), vertices.size() * sizeof(EntityJsonModel::Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    mesh.indices = Upload(indices.data(), indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    mesh.indexCount = static_cast<uint32_t>(indices.size());
}

void HandModel::CreatePipeline(const Engine::ShaderLibrary& shaders)
{
    VkShaderModule vertModule = CreateShaderModule(shaders, "hand.vert");
    VkShaderModule fragModule = VK_NULL_HANDLE;
    try
    {
        fragModule = CreateShaderModule(shaders, "hand.frag");
    }
    catch (...)
    {
        vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
        throw;
    }
    auto destroyModules = [&]
    {
        vkDestroyShaderModule(GetDevice(), fragModule, nullptr);
        vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
    };

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{ 0, sizeof(EntityJsonModel::Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EntityJsonModel::Vertex, position)) };
    attributes[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EntityJsonModel::Vertex, normal)) };
    attributes[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(EntityJsonModel::Vertex, uv)) };

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

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

    // No culling: the box is convex and depth-tested, and the transform chain
    // mixes handedness enough that winding is not worth getting wrong.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    if (pipelineLayout == VK_NULL_HANDLE)
    {
        VkDescriptorSetLayout skinLayout = EntityRenderer::GetSkinSetLayout();
        VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ArmPush) };
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &skinLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(GetDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        {
            destroyModules();
            throw std::runtime_error("[ERROR] HandModel: failed to create the pipeline layout.");
        }
    }

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
    pipelineInfo.renderPass = GetRenderPass();
    pipelineInfo.subpass = 0;
    VkResult result = vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    destroyModules();
    if (result != VK_SUCCESS)
    {
        pipeline = VK_NULL_HANDLE;
        throw std::runtime_error("[ERROR] HandModel: failed to create the graphics pipeline.");
    }
}

void HandModel::DrawArm(VkCommandBuffer cmd, const ArmMesh& mesh, const glm::mat4& mvp, float brightness)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet skinSet = EntityRenderer::GetLocalSkinSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &skinSet, 0, nullptr);
    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(cmd, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    ArmPush push{ mvp, glm::vec4(brightness, 0.0f, 0.0f, 0.0f) };
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
}

void HandModel::Record(Engine::FrameContext& frame)
{
    Application* state = frame.state;
    if (!ready || state == nullptr || state->itemRenderer == nullptr) return;

    HandScene::Frame hands;
    if (!scene.Update(frame, true, hands)) return;

    VkCommandBuffer cmd = frame.commandBuffer;
    float width = static_cast<float>(frame.extent.width);
    float height = static_cast<float>(frame.extent.height);
    VkViewport handViewport{ 0.0f, 0.0f, width, height, 0.0f, HandScene::MAX_DEPTH };
    vkCmdSetViewport(cmd, 0, 1, &handViewport);

    for (const HandScene::Arm& arm : hands.arms)
    {
        DrawArm(cmd, arm.right ? rightArmMesh : leftArmMesh, arm.mvp, hands.handLight);
    }
    for (const HandScene::Item& item : hands.items)
    {
        state->itemRenderer->RecordItem(cmd, item.name, item.context, item.placement, hands.proj,
            item.pivotRotation, item.darkness, item.glint);
    }

    VkViewport fullViewport{ 0.0f, 0.0f, width, height, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &fullViewport);
}

} // namespace Volcano
