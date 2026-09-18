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

namespace Volcano {

namespace {

struct Vertex { glm::vec3 position; }; // Unit cube corner, 0..1 per axis.

// One box (a block's collision shape can have more than one — a stair,
// say — so RecordDraw issues one draw per box) plus the color to tint it.
// worldMin/worldMax are already in world space (block origin + the shape's
// own local box — see RecordDraw), so the shader only interpolates across
// them; vec4 rather than vec3 throughout to sidestep push-constant
// alignment rules, same convention entity_textured.vert's PushConstants use.
struct PushConstants {
    glm::vec4 worldMin;
    glm::vec4 worldMax;
    glm::vec4 color;
};

VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
VkPipeline g_fillPipeline = VK_NULL_HANDLE; // TRIANGLE_LIST, alpha-blended.
VkPipeline g_linePipeline = VK_NULL_HANDLE; // LINE_LIST, alpha-blended.

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

// Builds the one shared unit cube (8 corners) plus its two index lists — a
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

    // Winding doesn't matter — the fill pipeline disables backface culling
    // (see CreatePipelines) so the highlight still shows from inside a
    // block the camera is pressed up against.
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

VkShaderModule CreateShaderModuleFromFile(const std::string& path)
{
    std::vector<char> code = ReadFile(path);
    if (code.empty())
    {
        throw std::runtime_error("[ERROR] Failed to read shader file: " + path);
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;
    if (vkCreateShaderModule(GetDevice(), &createInfo, nullptr, &module) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create shader module: " + path);
    }
    return module;
}

// One shared pipeline layout (camera set + a push constant), and two
// pipelines off the same shaders/fixed-function state differing only in
// topology — LINE_LIST for the outline, TRIANGLE_LIST for the fill.
void CreatePipelines()
{
    VkShaderModule vertModule = CreateShaderModuleFromFile("resources/shaders/selection.vert.spv");
    VkShaderModule fragModule = CreateShaderModuleFromFile("resources/shaders/selection.frag.spv");

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

    // No culling — the fill is meant to be visible from any angle,
    // including from inside a block the camera is pressed up against.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth-tested against the terrain/entity/non-cubic passes already in
    // the buffer (so a wall correctly hides the highlight behind it), but
    // never writes depth itself — it's an overlay, not real geometry.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Both the fill and the outline are translucent (selectionFill/
    // selectionOutline's own alpha) — standard alpha blend.
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
        throw std::runtime_error("[ERROR] Failed to create selection fill pipeline.");
    }

    VkPipelineInputAssemblyStateCreateInfo lineAssembly = triangleAssembly;
    lineAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    VkGraphicsPipelineCreateInfo linePipelineInfo = pipelineInfo;
    linePipelineInfo.pInputAssemblyState = &lineAssembly;
    if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &linePipelineInfo, nullptr, &g_linePipeline) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create selection line pipeline.");
    }

    vkDestroyShaderModule(GetDevice(), fragModule, nullptr);
    vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
}

} // namespace

void SelectionRenderer::Init()
{
    UploadMesh();
    CreatePipelines();
    Log::Info("[INFO] Selection renderer initialized.");
}

void SelectionRenderer::Shutdown()
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

    vmaDestroyBuffer(vmaAllocator, g_vertexBuffer.buffer, g_vertexBuffer.allocation);
    vmaDestroyBuffer(vmaAllocator, g_fillIndexBuffer.buffer, g_fillIndexBuffer.allocation);
    vmaDestroyBuffer(vmaAllocator, g_lineIndexBuffer.buffer, g_lineIndexBuffer.allocation);
}

void SelectionRenderer::RecordDraw(VkCommandBuffer cmd, uint32_t frameIndex, GlobalState* state)
{
    std::optional<TargetedBlock> target = state->interaction->GetTargetedBlock();
    if (!target) return;

    Block block = state->world->GetBlock(target->position.x, target->position.y, target->position.z);
    BlockRegistry::CollisionBoxes shape = BlockRegistry::GetCollisionBoxes(block);
    if (shape.count == 0) return; // Shouldn't happen (only solid blocks get targeted) — nothing to outline either way.

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
