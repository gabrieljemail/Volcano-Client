#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>
#include <glm/gtc/matrix_transform.hpp>
#include "ShadowPass.hpp"
#include "../ShaderLibrary.hpp"
#include "../../VulkanInit.hpp"
#include "../../models/PackedVertex.hpp"
#include "../../misc/MiscVertex.hpp"
#include "../../terrain/VisibleChunkController.hpp"
#include "../../../GlobalState.hpp"
#include "../../../Logger.hpp"

namespace Volcano::Engine::Passes {

namespace {

// How far toward the sun, past the fitted sphere, casters are still drawn, so
// a mountain behind the view can shadow what's in it.
constexpr float CASTER_MARGIN = 256.0f;

// Below this the sun is too low for a useful map.
constexpr float MIN_SUN_HEIGHT = 0.02f;

// Closest the slice starts, matching the camera's near plane.
constexpr float SLICE_NEAR = 0.1f;

// Three cascades: how much of the view distance each one covers (cumulative).
constexpr uint32_t CASCADE_COUNT = 3;
constexpr float CASCADE_SPLIT_FRACTIONS[CASCADE_COUNT] = { 0.08f, 0.30f, 1.0f };

// Depth bias against shadow acne on terrain faces.
constexpr float DEPTH_BIAS_CONSTANT = 1.25f;
constexpr float DEPTH_BIAS_SLOPE = 1.75f;

// Must match shadow.vert's Push block.
struct Push {
    glm::mat4 lightMvp;
};

} // namespace

void ShadowPass::Create(const ShaderLibrary&)
{
    // Nothing until shadows are enabled: no resources are held while off.
}

void ShadowPass::ReloadShaders(const ShaderLibrary& shaders)
{
    DestroyPipeline();
    if (target.IsValid()) CreatePipeline(shaders);
}

void ShadowPass::Destroy()
{
    ReleaseResources();
}

void ShadowPass::ReleaseResources()
{
    active = false;
    hasMap = false;
    if (vulkan == nullptr) return;

    // The device must be idle before the pipeline and target go.
    if (pipeline != VK_NULL_HANDLE || target.IsValid()) vulkan->WaitIdle();
    DestroyPipeline();
    vulkan->DestroyDepthTarget(target);
}

bool ShadowPass::EnsureTarget(uint32_t size)
{
    if (target.IsValid() && target.width == size) return true;

    vulkan->WaitIdle(); // The old pipeline and map may be in flight.
    DestroyPipeline();
    vulkan->DestroyDepthTarget(target);
    target = vulkan->CreateDepthTarget(size, size, true, CASCADE_COUNT);
    return target.IsValid();
}

glm::mat4 ShadowPass::FitSun(const SceneView& scene, glm::vec3 towardSun, float nearDistance, float farDistance, uint32_t mapSize,
    float& range, float& worldTexel) const
{
    // The camera's frustum slice, from its own matrices: eye, axes and the
    // field of view the projection encodes.
    glm::mat4 inverseView = glm::inverse(scene.view);
    glm::vec3 eye = glm::vec3(inverseView[3]);
    glm::vec3 right = glm::normalize(glm::vec3(inverseView[0]));
    glm::vec3 up = glm::normalize(glm::vec3(inverseView[1]));
    glm::vec3 forward = -glm::normalize(glm::vec3(inverseView[2]));
    float tanX = 1.0f / std::abs(scene.proj[0][0]);
    float tanY = 1.0f / std::abs(scene.proj[1][1]);

    glm::vec3 corners[8];
    int index = 0;
    for (float depth : { std::max(nearDistance, SLICE_NEAR), farDistance })
    {
        for (float sx : { -1.0f, 1.0f })
        {
            for (float sy : { -1.0f, 1.0f })
            {
                corners[index++] = eye + forward * depth + right * (sx * tanX * depth) + up * (sy * tanY * depth);
            }
        }
    }

    // A bounding sphere, so the box doesn't change size as the camera turns.
    // Rounded up to whole 8-block steps so a changing field of view (sprint
    // widening) doesn't resize the map every frame, which would make it shimmer.
    glm::vec3 center(0.0f);
    for (const glm::vec3& corner : corners) center += corner;
    center /= 8.0f;
    float radius = 0.0f;
    for (const glm::vec3& corner : corners) radius = std::max(radius, glm::length(corner - center));
    radius = std::ceil(radius / 8.0f) * 8.0f;

    // The light's orientation is anchored at the world origin, NOT at the
    // camera, so light-space positions don't slide as the camera moves. The
    // box is then snapped to whole texels in that fixed space: the same world
    // point always lands on the same texel, which is what stops the shimmer.
    glm::vec3 lightUp = std::abs(towardSun.y) > 0.99f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::mat4 lightView = glm::lookAt(glm::vec3(0.0f), -towardSun, lightUp);
    glm::vec3 lightCenter = glm::vec3(lightView * glm::vec4(center, 1.0f));

    float texel = 2.0f * radius / static_cast<float>(mapSize);
    float snappedX = std::floor(lightCenter.x / texel) * texel;
    float snappedY = std::floor(lightCenter.y / texel) * texel;

    // View-space z is negative forward, so a point's distance along the light
    // is -z. Casters toward the sun (smaller distance) are included up to the margin.
    float centerDistance = -lightCenter.z;
    float nearZ = centerDistance - (radius + CASTER_MARGIN);
    float farZ = centerDistance + radius;

    range = farZ - nearZ;
    worldTexel = texel;
    glm::mat4 lightProj = glm::orthoRH_ZO(snappedX - radius, snappedX + radius, snappedY - radius, snappedY + radius, nearZ, farZ);
    return lightProj * lightView;
}

bool ShadowPass::ShouldRecompute(const FrameContext& frame) const
{
    if (!hasMap || settings.distance != builtDistance) return true;

    // The last map is reused until the chosen tick counter moves on.
    switch (settings.recomputeEvent)
    {
        case ShadowRecomputeEvents::CLIENT_TICK: return frame.state->clientTicks.load() != lastTickStamp;
        case ShadowRecomputeEvents::SERVER_TICK: return frame.state->serverTicks.load() != lastTickStamp;
        default: return true;
    }
}

void ShadowPass::Record(FrameContext& frame)
{
    bool wasEnabled = settings.enabled;
    settings = ShadowSettings::Load(*frame.state->config);
    active = false;

    vulkan = frame.engine->GetVulkan();
    if (vulkan == nullptr) return;

    if (!settings.enabled)
    {
        ReleaseResources();
        return;
    }
    (void)wasEnabled;

    const SceneView& scene = *frame.scene;
    glm::vec3 towardSun = glm::normalize(scene.sunDirection);
    if (scene.menuBackground || towardSun.y < MIN_SUN_HEIGHT)
    {
        hasMap = false;
        return;
    }

    uint32_t mapSize = settings.MapSize();
    bool recreated = !target.IsValid() || target.width != mapSize;
    if (!EnsureTarget(mapSize)) return;
    if (recreated)
    {
        hasMap = false;
        vulkan->SetShadowMap(target);
    }
    if (pipeline == VK_NULL_HANDLE) CreatePipeline(frame.engine->GetShaderLibrary());

    // Shadows fade in with the sun's height; long low-sun shadows only look wrong.
    cascades.darkness = 0.55f * glm::smoothstep(MIN_SUN_HEIGHT, 0.3f, towardSun.y);
    cascades.mapTexel = 1.0f / static_cast<float>(target.width);
    cascades.debugView = settings.debugView;
    cascades.count = CASCADE_COUNT;

    if (ShouldRecompute(frame))
    {
        // Each cascade covers a slice of the view distance; the splits grow
        // geometrically so the near cascades stay tight.
        float distance = static_cast<float>(settings.distance);
        float previousFar = 0.0f;
        for (uint32_t i = 0; i < CASCADE_COUNT; i++)
        {
            float farDistance = distance * CASCADE_SPLIT_FRACTIONS[i];
            cascades.splitFar[i] = farDistance;
            cascades.viewProj[i] = FitSun(scene, towardSun, previousFar, farDistance, mapSize, cascades.range[i], cascades.worldTexel[i]);
            previousFar = farDistance;
        }

        terrainCasters = 0;
        cutoutCasters = 0;
        for (uint32_t i = 0; i < CASCADE_COUNT; i++) RecordCascade(frame, i);

        builtDistance = settings.distance;
        hasMap = true;
        switch (settings.recomputeEvent)
        {
            case ShadowRecomputeEvents::CLIENT_TICK: lastTickStamp = frame.state->clientTicks.load(); break;
            case ShadowRecomputeEvents::SERVER_TICK: lastTickStamp = frame.state->serverTicks.load(); break;
            default: break;
        }

        // With the debug view on, say what the pass drew about once a second.
        if (settings.debugView)
        {
            static auto lastLog = std::chrono::steady_clock::now() - std::chrono::seconds(5);
            auto now = std::chrono::steady_clock::now();
            if (now - lastLog >= std::chrono::seconds(1))
            {
                lastLog = now;
                Log::Info("[SHADOW] casters terrain=" + std::to_string(terrainCasters) + " cutout=" + std::to_string(cutoutCasters)
                    + " map=" + std::to_string(target.width) + "px texel(blocks)="
                    + std::to_string(cascades.worldTexel[0]) + "/" + std::to_string(cascades.worldTexel[1]) + "/"
                    + std::to_string(cascades.worldTexel[2]) + " splits="
                    + std::to_string(cascades.splitFar[0]) + "/" + std::to_string(cascades.splitFar[1]) + "/"
                    + std::to_string(cascades.splitFar[2]));
            }
        }
    }

    // The camera UBO is per frame, so it is handed over every frame, redrawn or not.
    vulkan->SetShadows(cascades);
    active = true;
}

// One cascade: its own layer of the map, its own light matrix.
void ShadowPass::RecordCascade(FrameContext& frame, uint32_t index)
{
    const glm::mat4& lightViewProj = cascades.viewProj[index];
    VkCommandBuffer cmd = frame.commandBuffer;

    VkClearValue clear{};
    clear.depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = target.renderPass;
    beginInfo.framebuffer = target.framebuffers[index];
    beginInfo.renderArea.extent = { target.width, target.height };
    beginInfo.clearValueCount = 1;
    beginInfo.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { 0, 0 }, { target.width, target.height } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    {
        // Locked against MeshingThread, same as the terrain pass.
        std::lock_guard<std::mutex> lock(frame.state->renderListMutex);
        std::vector<const Mesh*> casters =
            VisibleChunkController::GetVisibleMeshes(frame.state->renderList, lightViewProj, false);

        for (const Mesh* mesh : casters)
        {
            if (mesh->vertexBuffer == VK_NULL_HANDLE) continue;
            terrainCasters++;

            VkDeviceSize vertexOffset = mesh->vertexOffset;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertexBuffer, &vertexOffset);
            Push push{ lightViewProj * mesh->modelMatrix };
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

            if (mesh->indexCount > 0)
            {
                vkCmdBindIndexBuffer(cmd, mesh->indexBuffer, mesh->indexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
            }
            else
            {
                vkCmdDraw(cmd, mesh->vertexCount, 1, 0, 0);
            }
        }
    }

    // Cutout blocks (leaves, plants, glass, slabs): alpha-tested, after the
    // opaque terrain.
    if (cutoutPipeline != VK_NULL_HANDLE)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cutoutPipeline);
        VkDescriptorSet textureSet = vulkan->GetTextureSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cutoutLayout, 0, 1, &textureSet, 0, nullptr);

        std::lock_guard<std::mutex> lock(frame.state->nonCubicRenderListMutex);
        std::vector<const Mesh*> casters =
            VisibleChunkController::GetVisibleMeshes(frame.state->nonCubicRenderList, lightViewProj, false);
        for (const Mesh* mesh : casters)
        {
            if (mesh->vertexBuffer == VK_NULL_HANDLE) continue;
            cutoutCasters++;

            VkDeviceSize vertexOffset = mesh->vertexOffset;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertexBuffer, &vertexOffset);
            Push push{ lightViewProj * mesh->modelMatrix };
            vkCmdPushConstants(cmd, cutoutLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

            if (mesh->indexCount > 0)
            {
                vkCmdBindIndexBuffer(cmd, mesh->indexBuffer, mesh->indexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
            }
            else if (mesh->vertexCount > 0)
            {
                vkCmdDraw(cmd, mesh->vertexCount, 1, 0, 0);
            }
        }
    }

    // Entities, into the same open pass; only those near enough to matter to this cascade.
    EntityRenderer::RecordShadowCasters(cmd, pipelineLayout, entityPipelines, frame.state, lightViewProj,
        frame.scene->cameraPosition, cascades.splitFar[index], frame.frameIndex, index, frame.scene->indirectDraw);

    vkCmdEndRenderPass(cmd);
}

void ShadowPass::CreatePipeline(const ShaderLibrary& shaders)
{
    VkDevice dev = vulkan->GetVkDevice();
    VkShaderModule vertModule = CreateShaderModule(shaders, VERTEX_SHADER);

    // Depth only: a vertex stage and no fragment shader.
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vertModule;
    stage.pName = "main";

    VkVertexInputBindingDescription binding = PackedVertex::getBindingDescription();
    auto attributes = PackedVertex::getAttributeDescriptions();
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

    // No culling: both faces of thin geometry cast, and the light's winding
    // isn't the camera's.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // We could change this to backwards culling: cull visible faces and not backfaces.
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = DEPTH_BIAS_CONSTANT;
    rasterizer.depthBiasSlopeFactor = DEPTH_BIAS_SLOPE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    VkResult result = vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout);
    if (result == VK_SUCCESS)
    {
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 1;
        pipelineInfo.pStages = &stage;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = target.renderPass;
        pipelineInfo.subpass = 0;
        result = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        if (result != VK_SUCCESS) pipeline = VK_NULL_HANDLE;
    }
    else
    {
        pipelineLayout = VK_NULL_HANDLE;
    }

    vkDestroyShaderModule(dev, vertModule, nullptr);
    if (result != VK_SUCCESS)
    {
        DestroyPipeline();
        throw std::runtime_error("[ERROR] Failed to create shadow pipeline. Disabling shadows may fix this issue.");
    }

    // Entities use the same layout (one mat4) against the same render pass.
    try
    {
        EntityRenderer::CreateShadowPipelines(shaders, target.renderPass, pipelineLayout, entityPipelines);
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[ERROR] Entities won't cast shadows: ") + e.what());
    }

    try
    {
        CreateCutoutPipeline(shaders);
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[ERROR] Leaves and plants won't cast shadows: ") + e.what());
    }
}

// Cutout casters: MiscVertex in, alpha-tested against the block texture array.
void ShadowPass::CreateCutoutPipeline(const ShaderLibrary& shaders)
{
    VkDevice dev = vulkan->GetVkDevice();
    VkShaderModule vertModule = CreateShaderModule(shaders, "shadow_cutout.vert");
    VkShaderModule fragModule = VK_NULL_HANDLE;
    try
    {
        fragModule = CreateShaderModule(shaders, "shadow_cutout.frag");
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

    VkVertexInputBindingDescription binding = MiscVertex::getBindingDescription();
    auto attributes = MiscVertex::getAttributeDescriptions();
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

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // Crossed plants and thin geometry cast from both sides.
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = DEPTH_BIAS_CONSTANT;
    rasterizer.depthBiasSlopeFactor = DEPTH_BIAS_SLOPE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    VkDescriptorSetLayout setLayout = vulkan->GetTextureSetLayout();
    VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    VkResult result = vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &cutoutLayout);
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
        pipelineInfo.layout = cutoutLayout;
        pipelineInfo.renderPass = target.renderPass;
        pipelineInfo.subpass = 0;
        result = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &cutoutPipeline);
        if (result != VK_SUCCESS) cutoutPipeline = VK_NULL_HANDLE;
    }
    else
    {
        cutoutLayout = VK_NULL_HANDLE;
    }

    vkDestroyShaderModule(dev, fragModule, nullptr);
    vkDestroyShaderModule(dev, vertModule, nullptr);
    if (result != VK_SUCCESS) throw std::runtime_error("[ERROR] Failed to create cutout shadow pipeline.");
}

void ShadowPass::DestroyPipeline()
{
    if (vulkan == nullptr) return;
    VkDevice dev = vulkan->GetVkDevice();
    EntityRenderer::DestroyShadowPipelines(entityPipelines);
    if (cutoutPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, cutoutPipeline, nullptr);
    if (cutoutLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, cutoutLayout, nullptr);
    cutoutPipeline = VK_NULL_HANDLE;
    cutoutLayout = VK_NULL_HANDLE;
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
}

} // namespace Volcano::Engine::Passes
