#include <array>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>
#include "EntityRenderer.hpp"
#include "models/Entity.hpp"
#include "../VulkanInit.hpp"
#include "../../Helpers.hpp"
#include "../../Logger.hpp"

namespace Volcano {

namespace {

// Matches VulkanInit's fixed-size per-frame arrays (cameraSets, commandBuffers,
// etc.) — those are sized 3 even though maxFramesInFlight is 2, so we mirror
// that instead of introducing a second, possibly-inconsistent constant.
constexpr uint32_t FRAMES_IN_FLIGHT = 3;

// Local vertex format for the static placeholder cube — entities need their
// own vertex layout (position + normal) since they don't share the chunk
// mesh's packed-block format at all.
struct EntityVertex {
    glm::vec3 position;
    glm::vec3 normal;
};

// std430 layout: 3 vec4s, 16-byte aligned throughout.
struct GPUEntityInstance {
    glm::vec4 positionYaw;     // xyz: world position (feet), w: yaw radians.
    glm::vec4 halfExtentsHurt; // xyz: half bounding-box extents, w: hurt flash amount (0-1).
    glm::vec4 color;           // rgb: base tint, a: unused.
};

VkPipeline g_pipeline = VK_NULL_HANDLE;
VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_instanceSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_descriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_instanceSets[FRAMES_IN_FLIGHT];

AllocatedBuffer g_instanceBuffers[FRAMES_IN_FLIGHT];
void* g_instanceBuffersMapped[FRAMES_IN_FLIGHT];
AllocatedBuffer g_indirectBuffers[FRAMES_IN_FLIGHT];
void* g_indirectBuffersMapped[FRAMES_IN_FLIGHT];

AllocatedBuffer g_cubeVertexBuffer;
AllocatedBuffer g_cubeIndexBuffer;
uint32_t g_cubeIndexCount = 0;

// Allocates a buffer via VMA without registering it in VulkanInit's global
// `allocatedBuffers` list. EntityRenderer::Shutdown() destroys every buffer
// it creates itself (unmapping first where needed) — going through the
// global list here would mean both it and VulkanInit::Cleanup()'s own sweep
// try to destroy the same VkBuffer/VmaAllocation, which is a double free.
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
        throw std::runtime_error("[ERROR] EntityRenderer failed to allocate a buffer.");
    }
    return result;
}

// Builds a unit cube (local -0.5..0.5 on each axis) with 4 unique vertices
// per face (so each face keeps its own flat normal) — 24 vertices, 36
// indices. Winding follows the same convention as ChunkMesher/terrain.vert:
// per face, u x v == the face normal, and corners run
// (0,0)->(1,0)->(1,1)->(0,1), which is CCW seen from outside — required for
// VulkanInit's VK_FRONT_FACE_CLOCKWISE + VK_CULL_MODE_BACK_BIT rasterizer
// state (see CreateGraphicsPipeline's comment on why) to cull the right
// faces instead of turning every cube inside-out.
void BuildCubeMesh(std::vector<EntityVertex>& vertices, std::vector<uint32_t>& indices)
{
    struct Face { glm::vec3 normal; glm::vec3 u; glm::vec3 v; };
    const std::array<Face, 6> faces = {{
        { glm::vec3( 1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1) },
        { glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0) },
        { glm::vec3( 0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0) },
        { glm::vec3( 0,-1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1) },
        { glm::vec3( 0, 0, 1), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0) },
        { glm::vec3( 0, 0,-1), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0) },
    }};

    for (const Face& face : faces)
    {
        glm::vec3 center = face.normal * 0.5f;
        uint32_t base = static_cast<uint32_t>(vertices.size());

        vertices.push_back({ center - 0.5f * face.u - 0.5f * face.v, face.normal });
        vertices.push_back({ center + 0.5f * face.u - 0.5f * face.v, face.normal });
        vertices.push_back({ center + 0.5f * face.u + 0.5f * face.v, face.normal });
        vertices.push_back({ center - 0.5f * face.u + 0.5f * face.v, face.normal });

        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
}

void UploadCubeMesh()
{
    std::vector<EntityVertex> vertices;
    std::vector<uint32_t> indices;
    BuildCubeMesh(vertices, indices);
    g_cubeIndexCount = static_cast<uint32_t>(indices.size());

    VkDeviceSize vertexBytes = vertices.size() * sizeof(EntityVertex);
    VkDeviceSize indexBytes = indices.size() * sizeof(uint32_t);

    g_cubeVertexBuffer = CreateOwnedBuffer(vertexBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    g_cubeIndexBuffer = CreateOwnedBuffer(indexBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);

    AllocatedBuffer staging = CreateOwnedBuffer(vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    void* mapped;
    vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
    memcpy(mapped, vertices.data(), (size_t)vertexBytes);
    memcpy(static_cast<uint8_t*>(mapped) + vertexBytes, indices.data(), (size_t)indexBytes);
    vmaUnmapMemory(vmaAllocator, staging.allocation);

    VkCommandBuffer cmd = BeginOneShotCommands();

    VkBufferCopy vertexCopy{ 0, 0, vertexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, g_cubeVertexBuffer.buffer, 1, &vertexCopy);

    VkBufferCopy indexCopy{ vertexBytes, 0, indexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, g_cubeIndexBuffer.buffer, 1, &indexCopy);

    EndOneShotCommands(cmd);

    vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);
}

void CreateDescriptorLayoutAndPool()
{
    VkDescriptorSetLayoutBinding instanceBinding{};
    instanceBinding.binding = 0;
    instanceBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    instanceBinding.descriptorCount = 1;
    instanceBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &instanceBinding;
    if (vkCreateDescriptorSetLayout(GetDevice(), &layoutInfo, nullptr, &g_instanceSetLayout) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create entity instance descriptor set layout.");
    }

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FRAMES_IN_FLIGHT };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(GetDevice(), &poolInfo, nullptr, &g_descriptorPool) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create entity descriptor pool.");
    }
}

void CreatePerFrameBuffers()
{
    VkDeviceSize instanceBytes = EntityRenderer::MAX_VISIBLE_ENTITIES * sizeof(GPUEntityInstance);

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        g_instanceBuffers[i] = CreateOwnedBuffer(instanceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        vmaMapMemory(vmaAllocator, g_instanceBuffers[i].allocation, &g_instanceBuffersMapped[i]);

        g_indirectBuffers[i] = CreateOwnedBuffer(sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
        vmaMapMemory(vmaAllocator, g_indirectBuffers[i].allocation, &g_indirectBuffersMapped[i]);

        VkDrawIndexedIndirectCommand initialCommand{};
        initialCommand.indexCount = g_cubeIndexCount;
        initialCommand.instanceCount = 0;
        memcpy(g_indirectBuffersMapped[i], &initialCommand, sizeof(initialCommand));

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = g_descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &g_instanceSetLayout;
        if (vkAllocateDescriptorSets(GetDevice(), &allocInfo, &g_instanceSets[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("[ERROR] Failed to allocate entity instance descriptor set.");
        }

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = g_instanceBuffers[i].buffer;
        bufferInfo.range = instanceBytes;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = g_instanceSets[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(GetDevice(), 1, &write, 0, nullptr);
    }
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

void CreatePipeline()
{
    VkShaderModule vertModule = CreateShaderModuleFromFile("resources/shaders/entity.vert.spv");
    VkShaderModule fragModule = CreateShaderModuleFromFile("resources/shaders/entity.frag.spv");

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
    binding.stride = sizeof(EntityVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attributes{};
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(EntityVertex, position);
    attributes[1].binding = 0;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(EntityVertex, normal);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
    dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicStateInfo.pDynamicStates = dynamicStates.data();

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Same winding/cull convention as the terrain pipeline (see
    // CreateGraphicsPipeline's comment) — BuildCubeMesh's winding matches it.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Entities share the chunk pass's depth attachment within the same
    // subpass/render pass, so depth test/write here is what makes walls
    // correctly occlude entities behind them (and vice versa) regardless of
    // draw order — no extra wiring needed for that culling method.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkDescriptorSetLayout setLayouts[] = { cameraSetLayout, g_instanceSetLayout };
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
    if (vkCreatePipelineLayout(GetDevice(), &pipelineLayoutInfo, nullptr, &g_pipelineLayout) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create entity pipeline layout.");
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
    pipelineInfo.pDynamicState = &dynamicStateInfo;
    pipelineInfo.layout = g_pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &g_pipeline) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create entity graphics pipeline.");
    }

    vkDestroyShaderModule(GetDevice(), fragModule, nullptr);
    vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
}

// Left/Right/Bottom/Top frustum planes (Gribb/Hartmann) extracted from a
// combined view-projection matrix. Near/far are deliberately skipped: the
// rasterizer's own clip-space clipping already handles anything we'd miss
// there, and the distance-sorted MAX_VISIBLE_ENTITIES cap in RecordDraw is a
// more meaningful "too far/too much" limit than a math-derived far plane.
std::array<glm::vec4, 4> ExtractFrustumPlanes(const glm::mat4& m)
{
    std::array<glm::vec4, 4> planes = {
        glm::vec4(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]), // Left
        glm::vec4(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]), // Right
        glm::vec4(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]), // Bottom
        glm::vec4(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]), // Top
    };

    for (glm::vec4& p : planes)
    {
        float len = glm::length(glm::vec3(p));
        if (len > 0.0f) p /= len;
    }
    return planes;
}

bool SphereInFrustum(const std::array<glm::vec4, 4>& planes, const glm::vec3& center, float radius)
{
    for (const glm::vec4& p : planes)
    {
        if (glm::dot(glm::vec3(p), center) + p.w < -radius) return false;
    }
    return true;
}

} // namespace

void EntityRenderer::Init()
{
    UploadCubeMesh();
    CreateDescriptorLayoutAndPool();
    CreatePerFrameBuffers();
    CreatePipeline();

    Log::Info("[INFO] Entity renderer initialized.");
}

void EntityRenderer::Shutdown()
{
    if (g_pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(GetDevice(), g_pipeline, nullptr);
        g_pipeline = VK_NULL_HANDLE;
    }
    if (g_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(GetDevice(), g_pipelineLayout, nullptr);
        g_pipelineLayout = VK_NULL_HANDLE;
    }

    // Destroying the pool frees every set allocated from it.
    if (g_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(GetDevice(), g_descriptorPool, nullptr);
        g_descriptorPool = VK_NULL_HANDLE;
    }
    if (g_instanceSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(GetDevice(), g_instanceSetLayout, nullptr);
        g_instanceSetLayout = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        if (g_instanceBuffersMapped[i] != nullptr)
        {
            vmaUnmapMemory(vmaAllocator, g_instanceBuffers[i].allocation);
            g_instanceBuffersMapped[i] = nullptr;
        }
        if (g_instanceBuffers[i].buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(vmaAllocator, g_instanceBuffers[i].buffer, g_instanceBuffers[i].allocation);
            g_instanceBuffers[i] = AllocatedBuffer{};
        }

        if (g_indirectBuffersMapped[i] != nullptr)
        {
            vmaUnmapMemory(vmaAllocator, g_indirectBuffers[i].allocation);
            g_indirectBuffersMapped[i] = nullptr;
        }
        if (g_indirectBuffers[i].buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(vmaAllocator, g_indirectBuffers[i].buffer, g_indirectBuffers[i].allocation);
            g_indirectBuffers[i] = AllocatedBuffer{};
        }
    }

    if (g_cubeVertexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_cubeVertexBuffer.buffer, g_cubeVertexBuffer.allocation);
        g_cubeVertexBuffer = AllocatedBuffer{};
    }
    if (g_cubeIndexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_cubeIndexBuffer.buffer, g_cubeIndexBuffer.allocation);
        g_cubeIndexBuffer = AllocatedBuffer{};
    }
}

void EntityRenderer::RecordDraw(VkCommandBuffer cmd, uint32_t frameIndex, GlobalState* state,
    const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPosition)
{
    std::array<glm::vec4, 4> frustumPlanes = ExtractFrustumPlanes(proj * view);

    // Frustum-cull first, then keep only the closest MAX_VISIBLE_ENTITIES —
    // the "simple entity limit" culling method from the entity renderer
    // plan, applied after frustum culling so it prioritizes what's actually
    // in view instead of arbitrarily dropping visible entities in favor of
    // off-screen ones.
    auto now = std::chrono::steady_clock::now();

    std::vector<std::pair<float, const Entity*>> visible;
    {
        std::lock_guard<std::mutex> lock(state->entitiesMutex);
        visible.reserve(state->entities.size());

        for (const auto& [id, entity] : state->entities)
        {
            if (!entity.visible) continue;

            float halfWidth = entity.boundingBox.x * 0.5f;
            float halfHeight = entity.boundingBox.y * 0.5f;
            glm::vec3 center = entity.InterpolatedPosition(now) + glm::vec3(0.0f, halfHeight, 0.0f);
            float radius = glm::length(glm::vec3(halfWidth, halfHeight, halfWidth));

            if (!SphereInFrustum(frustumPlanes, center, radius)) continue;

            float distanceSq = glm::dot(center - cameraPosition, center - cameraPosition);
            visible.emplace_back(distanceSq, &entity);
        }

        if (visible.size() > MAX_VISIBLE_ENTITIES)
        {
            std::partial_sort(visible.begin(), visible.begin() + MAX_VISIBLE_ENTITIES, visible.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            visible.resize(MAX_VISIBLE_ENTITIES);
        }

        // Build the GPU instance buffer while still holding the lock, since
        // `visible` holds raw pointers into state->entities.
        std::vector<GPUEntityInstance> instances;
        instances.reserve(visible.size());
        for (const auto& [distanceSq, entity] : visible)
        {
            GPUEntityInstance gpu{};
            gpu.positionYaw = glm::vec4(entity->InterpolatedPosition(now), entity->InterpolatedYaw(now));
            gpu.halfExtentsHurt = glm::vec4(
                entity->boundingBox.x * 0.5f, entity->boundingBox.y * 0.5f, entity->boundingBox.x * 0.5f,
                entity->hurtAmount);
            gpu.color = glm::vec4(entity->color, 1.0f);
            instances.push_back(gpu);
        }

        if (!instances.empty())
        {
            memcpy(g_instanceBuffersMapped[frameIndex], instances.data(), instances.size() * sizeof(GPUEntityInstance));
        }
    }

    VkDrawIndexedIndirectCommand command{};
    command.indexCount = g_cubeIndexCount;
    command.instanceCount = static_cast<uint32_t>(visible.size());
    memcpy(g_indirectBuffersMapped[frameIndex], &command, sizeof(command));

    if (visible.empty()) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);

    VkDescriptorSet sets[] = { cameraSets[frameIndex], g_instanceSets[frameIndex] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 2, sets, 0, nullptr);

    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_cubeVertexBuffer.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(cmd, g_cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
}

} // namespace Volcano
