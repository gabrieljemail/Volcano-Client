#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>
#include "SelectionRenderer.hpp"
#include "../VulkanInit.hpp"
#include "../terrain/models/BlockRegistry.hpp"
#include "../../Helpers.hpp"
#include "../../Logger.hpp"
#include "../../interaction/InteractionManager.hpp"
#include "../entity/EntityRenderer.hpp"
#include <chrono>

namespace Volcano {

namespace {

struct Vertex { glm::vec3 position; }; // Unit cube corner, 0..1 per axis.

// One box (a block's collision shape can have more than one - a stair,
// say - so RecordDraw issues one draw per box) plus the color to tint it.
// worldMin/worldMax are already in world space (block origin + the shape's
// own local box
struct PushConstants {
    glm::vec4 worldMin;
    glm::vec4 worldMax;
    glm::vec4 color;
};

VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
VkPipeline g_fillPipeline = VK_NULL_HANDLE; // TRIANGLE_LIST, alpha-blended.
VkPipeline g_linePipeline = VK_NULL_HANDLE; // LINE_LIST, alpha-blended.

// The crack overlay: a textured unit cube (24 vertices, so each face gets its own UVs) drawn per shape box.
struct CrackVertex { glm::vec3 position; glm::vec2 uv; };
struct CrackPushConstants { glm::vec4 worldMin; glm::vec4 worldMax; };
VkPipelineLayout g_crackLayout = VK_NULL_HANDLE;
VkPipeline g_crackPipeline = VK_NULL_HANDLE;
AllocatedBuffer g_crackVertexBuffer;
AllocatedBuffer g_crackIndexBuffer;
constexpr uint32_t CRACK_INDEX_COUNT = 36;

AllocatedBuffer g_vertexBuffer;
AllocatedBuffer g_fillIndexBuffer;
AllocatedBuffer g_lineIndexBuffer;
constexpr uint32_t FILL_INDEX_COUNT = 36; // 6 faces * 2 triangles * 3.
constexpr uint32_t LINE_INDEX_COUNT = 24; // 12 edges * 2.

// See EntityRenderer.cpp's own copy of this exact helper for why this isn't
// shared/registered in VulkanInit's global allocatedBuffers list.
AllocatedBuffer CreateOwnedBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool isUMA)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = isUMA ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : 0;

    AllocatedBuffer result{};
    if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &result.info) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] SelectionRenderer failed to allocate a buffer.");
    }
    return result;
}

// Builds the one shared unit cube (8 corners) plus its two index lists - a
// TRIANGLE_LIST one for the 6-face fill and a LINE_LIST one for the 12-edge
// outline. Uploaded once; every draw just repositions/rescales/recolors it
// via push constants (see RecordDraw), so this never runs again after Init().
void UploadMesh()
{
    // Indexed by bit0=x, bit1=y, bit2=z.
    std::array<Vertex, 8> vertices{{
        Vertex{{0.0f, 0.0f, 0.0f}}, Vertex{{1.0f, 0.0f, 0.0f}},
        Vertex{{0.0f, 1.0f, 0.0f}}, Vertex{{1.0f, 1.0f, 0.0f}},
        Vertex{{0.0f, 0.0f, 1.0f}}, Vertex{{1.0f, 0.0f, 1.0f}},
        Vertex{{0.0f, 1.0f, 1.0f}}, Vertex{{1.0f, 1.0f, 1.0f}},
    }};

    // Winding doesn't matter
    std::array<uint16_t, FILL_INDEX_COUNT> fillIndices{{
        0,2,6, 0,6,4, // -X
        1,5,7, 1,7,3, // +X
        0,1,5, 0,5,4, // -Y
        2,6,7, 2,7,3, // +Y
        0,1,3, 0,3,2, // -Z
        4,5,7, 4,7,6, // +Z
    }};

    std::array<uint16_t, LINE_INDEX_COUNT> lineIndices{{
        0,1, 0,2, 0,4, 1,3, 1,5, 2,3,
        2,6, 3,7, 4,5, 4,6, 5,7, 6,7,
    }};

    VkDeviceSize vertexBytes = vertices.size() * sizeof(Vertex);
    VkDeviceSize fillBytes = fillIndices.size() * sizeof(uint16_t);
    VkDeviceSize lineBytes = lineIndices.size() * sizeof(uint16_t);

    g_vertexBuffer = CreateOwnedBuffer(vertexBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    g_fillIndexBuffer = CreateOwnedBuffer(fillBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);
    g_lineIndexBuffer = CreateOwnedBuffer(lineBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);

    AllocatedBuffer staging = CreateOwnedBuffer(vertexBytes + fillBytes + lineBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    void* mapped;
    vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
    memcpy(mapped, vertices.data(), (size_t)vertexBytes);
    memcpy(static_cast<uint8_t*>(mapped) + vertexBytes, fillIndices.data(), (size_t)fillBytes);
    memcpy(static_cast<uint8_t*>(mapped) + vertexBytes + fillBytes, lineIndices.data(), (size_t)lineBytes);
    vmaUnmapMemory(vmaAllocator, staging.allocation);

    VkCommandBuffer cmd = BeginOneShotCommands();

    VkBufferCopy vertexCopy{ 0, 0, vertexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, g_vertexBuffer.buffer, 1, &vertexCopy);
    VkBufferCopy fillCopy{ vertexBytes, 0, fillBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, g_fillIndexBuffer.buffer, 1, &fillCopy);
    VkBufferCopy lineCopy{ vertexBytes + fillBytes, 0, lineBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, g_lineIndexBuffer.buffer, 1, &lineCopy);

    EndOneShotCommands(cmd);

    vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);
}

void UploadCrackMesh()
{
    std::array<CrackVertex, 24> vertices{};
    std::array<uint16_t, CRACK_INDEX_COUNT> indices{};
    for (int face = 0; face < 6; face++)
    {
        int normalAxis = face / 2, uAxis = (normalAxis + 1) % 3, vAxis = (normalAxis + 2) % 3;
        const float corners[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int c = 0; c < 4; c++)
        {
            CrackVertex& vertex = vertices[static_cast<size_t>(face * 4 + c)];
            vertex.position[normalAxis] = static_cast<float>(face % 2);
            vertex.position[uAxis] = corners[c][0];
            vertex.position[vAxis] = corners[c][1];
            vertex.uv = glm::vec2(corners[c][0], 1.0f - corners[c][1]);
        }
        const uint16_t base = static_cast<uint16_t>(face * 4);
        const uint16_t order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < 6; i++) indices[static_cast<size_t>(face * 6 + i)] = static_cast<uint16_t>(base + order[i]);
    }

    VkDeviceSize vertexBytes = vertices.size() * sizeof(CrackVertex);
    VkDeviceSize indexBytes = indices.size() * sizeof(uint16_t);
    g_crackVertexBuffer = CreateOwnedBuffer(vertexBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    g_crackIndexBuffer = CreateOwnedBuffer(indexBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);

    AllocatedBuffer staging = CreateOwnedBuffer(vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    void* mapped;
    vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
    memcpy(mapped, vertices.data(), static_cast<size_t>(vertexBytes));
    memcpy(static_cast<uint8_t*>(mapped) + vertexBytes, indices.data(), static_cast<size_t>(indexBytes));
    vmaUnmapMemory(vmaAllocator, staging.allocation);

    VkCommandBuffer cmd = BeginOneShotCommands();
    VkBufferCopy vertexCopy{ 0, 0, vertexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, g_crackVertexBuffer.buffer, 1, &vertexCopy);
    VkBufferCopy indexCopy{ vertexBytes, 0, indexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, g_crackIndexBuffer.buffer, 1, &indexCopy);
    EndOneShotCommands(cmd);
    vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);
}

// Multiplicative overlay (dst * src * 2) over the block, depth-tested with the selection's bias.
void CreateCrackPipeline(const Engine::ShaderLibrary& shaders)
{
    VkShaderModule vertModule = CreateShaderModule(shaders, "crack.vert");
    VkShaderModule fragModule = VK_NULL_HANDLE;
    try
    {
        fragModule = CreateShaderModule(shaders, "crack.frag");
    }
    catch (...)
    {
        vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
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

    VkVertexInputBindingDescription binding{ 0, sizeof(CrackVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[2] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(CrackVertex, position)) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(CrackVertex, uv)) } };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamics;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = -4.0f;
    rasterizer.depthBiasSlopeFactor = -4.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;

    VkDescriptorSetLayout setLayouts[] = { cameraSetLayout, EntityRenderer::GetSkinSetLayout() };
    VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(CrackPushConstants) };
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VkResult result = vkCreatePipelineLayout(GetDevice(), &layoutInfo, nullptr, &g_crackLayout);
    if (result == VK_SUCCESS)
    {
        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &rasterizer;
        info.pMultisampleState = &multisampling;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &blending;
        info.pDynamicState = &dynamicState;
        info.layout = g_crackLayout;
        info.renderPass = renderPass;
        result = vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &g_crackPipeline);
        if (result != VK_SUCCESS) g_crackPipeline = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(GetDevice(), fragModule, nullptr);
    vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
    if (result != VK_SUCCESS) Log::Error("[ERROR] Block-break crack pipeline unavailable.");
}

// Draws the crack texture for `stage` over every box of the block at `position`.
void DrawCracks(VkCommandBuffer cmd, uint32_t frameIndex, GlobalState* state, const glm::ivec3& position, int stage)
{
    static int lastLoggedStage = -1;
    if (g_crackPipeline == VK_NULL_HANDLE)
    {
        if (lastLoggedStage != -2) Log::Error("[CRACK] No crack pipeline; nothing to draw.");
        lastLoggedStage = -2;
        return;
    }
    VkDescriptorSet crackSet = EntityRenderer::GetCrackSet(stage);
    Block block = state->world->GetBlock(position.x, position.y, position.z);
    BlockRegistry::CollisionBoxes shape = BlockRegistry::GetCollisionBoxes(block);
    if (stage != lastLoggedStage)
    {
        lastLoggedStage = stage;
        Log::Info("[CRACK] Stage " + std::to_string(stage) + " at " + std::to_string(position.x) + "," + std::to_string(position.y)
            + "," + std::to_string(position.z) + ": texture " + (crackSet != VK_NULL_HANDLE ? "ok" : "MISSING")
            + ", " + std::to_string(shape.count) + " shape box(es).");
    }
    if (crackSet == VK_NULL_HANDLE || shape.count == 0) return;

    constexpr float INFLATE = 0.002f;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_crackPipeline);
    VkDescriptorSet sets[] = { cameraSets[frameIndex], crackSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_crackLayout, 0, 2, sets, 0, nullptr);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_crackVertexBuffer.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, g_crackIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);
    for (int i = 0; i < shape.count; i++)
    {
        glm::vec3 origin(position);
        CrackPushConstants pc{ glm::vec4(origin + shape.boxes[static_cast<size_t>(i)].min - glm::vec3(INFLATE), 0.0f),
            glm::vec4(origin + shape.boxes[static_cast<size_t>(i)].max + glm::vec3(INFLATE), 0.0f) };
        vkCmdPushConstants(cmd, g_crackLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, CRACK_INDEX_COUNT, 1, 0, 0, 0);
    }
}

// One shared pipeline layout (camera set + a push constant), and two
// pipelines off the same shaders/fixed-function state differing only in
// topology
void CreatePipelines(const Engine::ShaderLibrary& shaders)
{
    VkShaderModule vertModule = CreateShaderModule(shaders, "selection.vert");
    VkShaderModule fragModule = VK_NULL_HANDLE;
    try
    {
        fragModule = CreateShaderModule(shaders, "selection.frag");
    }
    catch (...)
    {
        vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
        throw;
    }

    // Modules are only needed until the pipelines exist
    auto destroyModules = [&]()
    {
        vkDestroyShaderModule(GetDevice(), fragModule, nullptr);
        vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
    };

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = { vertStage, fragStage };

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attribute{};
    attribute.binding = 0;
    attribute.location = 0;
    attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
    attribute.offset = offsetof(Vertex, position);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attribute;

    std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
    dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicStateInfo.pDynamicStates = dynamicStates.data();

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // No culling
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;

    // RecordDraw's own INFLATE nudge is a fixed world-space offset, which
    // shrinks to nothing in depth-buffer terms at range (standard
    // perspective Z loses precision with distance) - that's why the
    // flicker got worse the farther out the targeted block was. Depth bias
    // operates directly in the depth buffer's own units instead, so it
    // stays effective at any distance. Negative pulls the overlay closer
    // than the coplanar block face it's drawn against.
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = -4.0f;
    rasterizer.depthBiasSlopeFactor = -4.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth-tested against the terrain/entity/non-cubic passes already in
    // the buffer (so a wall correctly hides the highlight behind it), but
    // never writes depth itself
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Both the fill and the outline are translucent (selectionFill/
    // selectionOutline's own alpha) - standard alpha blend.
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &cameraSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    if (vkCreatePipelineLayout(GetDevice(), &pipelineLayoutInfo, nullptr, &g_pipelineLayout) != VK_SUCCESS)
    {
        destroyModules();
        throw std::runtime_error("[ERROR] Failed to create selection pipeline layout.");
    }

    VkPipelineInputAssemblyStateCreateInfo triangleAssembly{};
    triangleAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    triangleAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &triangleAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicStateInfo;
    pipelineInfo.layout = g_pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &g_fillPipeline) != VK_SUCCESS)
    {
        g_fillPipeline = VK_NULL_HANDLE;
        destroyModules();
        throw std::runtime_error("[ERROR] Failed to create selection fill pipeline.");
    }

    VkPipelineInputAssemblyStateCreateInfo lineAssembly = triangleAssembly;
    lineAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    VkGraphicsPipelineCreateInfo linePipelineInfo = pipelineInfo;
    linePipelineInfo.pInputAssemblyState = &lineAssembly;
    if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &linePipelineInfo, nullptr, &g_linePipeline) != VK_SUCCESS)
    {
        g_linePipeline = VK_NULL_HANDLE;
        destroyModules();
        throw std::runtime_error("[ERROR] Failed to create selection line pipeline.");
    }

    destroyModules();
}

void DestroyPipelines()
{
    if (g_fillPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(GetDevice(), g_fillPipeline, nullptr);
        g_fillPipeline = VK_NULL_HANDLE;
    }
    if (g_linePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(GetDevice(), g_linePipeline, nullptr);
        g_linePipeline = VK_NULL_HANDLE;
    }
    if (g_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(GetDevice(), g_pipelineLayout, nullptr);
        g_pipelineLayout = VK_NULL_HANDLE;
    }
    if (g_crackPipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), g_crackPipeline, nullptr);
    if (g_crackLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(GetDevice(), g_crackLayout, nullptr);
    g_crackPipeline = VK_NULL_HANDLE;
    g_crackLayout = VK_NULL_HANDLE;
}

} // namespace

void SelectionRenderer::Init(const Engine::ShaderLibrary& shaders)
{
    UploadMesh();
    UploadCrackMesh();
    CreatePipelines(shaders);
    CreateCrackPipeline(shaders);
    Log::Info("Selection renderer initialized.");
}

void SelectionRenderer::ReloadPipelines(const Engine::ShaderLibrary& shaders)
{
    DestroyPipelines();
    CreatePipelines(shaders);
    CreateCrackPipeline(shaders);
}

void SelectionRenderer::Shutdown()
{
    DestroyPipelines();

    // VMA treats a null buffer/allocation as a no-op, so resetting the
    // handles is all it takes to make a second Shutdown() harmless.
    vmaDestroyBuffer(vmaAllocator, g_vertexBuffer.buffer, g_vertexBuffer.allocation);
    vmaDestroyBuffer(vmaAllocator, g_fillIndexBuffer.buffer, g_fillIndexBuffer.allocation);
    vmaDestroyBuffer(vmaAllocator, g_lineIndexBuffer.buffer, g_lineIndexBuffer.allocation);
    vmaDestroyBuffer(vmaAllocator, g_crackVertexBuffer.buffer, g_crackVertexBuffer.allocation);
    vmaDestroyBuffer(vmaAllocator, g_crackIndexBuffer.buffer, g_crackIndexBuffer.allocation);
    g_crackVertexBuffer = AllocatedBuffer{};
    g_crackIndexBuffer = AllocatedBuffer{};
    g_vertexBuffer = AllocatedBuffer{};
    g_fillIndexBuffer = AllocatedBuffer{};
    g_lineIndexBuffer = AllocatedBuffer{};
}

void SelectionRenderer::RecordDraw(VkCommandBuffer cmd, uint32_t frameIndex, GlobalState* state)
{
    std::optional<TargetedBlock> target = state->interaction->GetTargetedBlock();

    // Cracks first, so the outline draws over them: our own mining, then everyone else's.
    int32_t ticksRequired = state->miningTicksRequired.load(std::memory_order_relaxed);
    if (target && ticksRequired > 0)
    {
        int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        float ticks = static_cast<float>(nowNs - state->miningStartNs.load(std::memory_order_relaxed)) * 1e-9f * 20.0f;
        int stage = std::clamp(static_cast<int>(ticks / static_cast<float>(ticksRequired) * 10.0f), 0, 9);
        DrawCracks(cmd, frameIndex, state, target->position, stage);
    }
    {
        std::lock_guard<std::mutex> lock(state->blockBreaksMutex);
        for (const auto& [breaker, progress] : state->blockBreaks)
        {
            DrawCracks(cmd, frameIndex, state, progress.position, progress.stage);
        }
    }

    if (!target) return;

    Block block = state->world->GetBlock(target->position.x, target->position.y, target->position.z);
    BlockRegistry::CollisionBoxes shape = BlockRegistry::GetCollisionBoxes(block);
    if (shape.count == 0) return; // Shouldn't happen (only solid blocks get targeted)

    Color4i outline = state->interaction->GetSelectionOutlineColor();
    Color4i fill = state->interaction->GetSelectionFillColor();
    glm::vec4 outlineColor(outline.r / 255.0f, outline.g / 255.0f, outline.b / 255.0f, outline.a);
    glm::vec4 fillColor(fill.r / 255.0f, fill.g / 255.0f, fill.b / 255.0f, fill.a);

    glm::vec3 blockOrigin(target->position);
    // Nudges the box just outside the block's real face so its coplanar
    // fill/outline don't z-fight against the block itself.
    constexpr float INFLATE = 0.002f;

    VkDescriptorSet sets[] = { cameraSets[frameIndex] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 1, sets, 0, nullptr);

    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_vertexBuffer.buffer, &vertexOffset);

    for (int i = 0; i < shape.count; i++)
    {
        glm::vec3 worldMin = blockOrigin + shape.boxes[static_cast<size_t>(i)].min - glm::vec3(INFLATE);
        glm::vec3 worldMax = blockOrigin + shape.boxes[static_cast<size_t>(i)].max + glm::vec3(INFLATE);

        PushConstants pc{ glm::vec4(worldMin, 0.0f), glm::vec4(worldMax, 0.0f), fillColor };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fillPipeline);
        vkCmdPushConstants(cmd, g_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdBindIndexBuffer(cmd, g_fillIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, FILL_INDEX_COUNT, 1, 0, 0, 0);

        pc.color = outlineColor;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_linePipeline);
        vkCmdPushConstants(cmd, g_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdBindIndexBuffer(cmd, g_lineIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, LINE_INDEX_COUNT, 1, 0, 0, 0);
    }
}

} // namespace Volcano
