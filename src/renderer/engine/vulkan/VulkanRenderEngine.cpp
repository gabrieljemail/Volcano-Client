#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include "VulkanRenderEngine.hpp"
#include "../passes/EntityPass.hpp"
#include "../passes/ModeledBEPass.hpp"
#include "../passes/GUIPass.hpp"
#include "../passes/HandPass.hpp"
#include "../passes/ItemPass.hpp"
#include "../passes/NonCubicPass.hpp"
#include "../passes/SelectionPass.hpp"
#include "../passes/ShaderPackPass.hpp"
#include "../passes/SkyboxPass.hpp"
#include "../passes/CloudPass.hpp"
#include "../passes/ParticlePass.hpp"
#include "../passes/ShadowPass.hpp"
#include "../passes/TerrainPass.hpp"
#include "../../VulkanInit.hpp"
#include "../../TextureManager.hpp"
#include "../../models/CameraUBO.hpp"
#include "../../models/SlabBuffer.hpp"
#include "../../terrain/OcclusionSnapshot.hpp"
#include "../../Screenshot.hpp"
#include <utility>
#include <vector>
#include "../../gui/GUIController.hpp"
#include "../../../GlobalState.hpp"
#include "../../../Logger.hpp"

namespace Volcano::Engine::Vulkan {

void VulkanRenderEngine::Init()
{
    // Window, instance, device, swapchain, render pass, descriptor sets,
    // command buffers, sync objects - everything but pipelines.
    ::Init(state);
    initialized = true;

    shaders.SetActivePack(std::get<std::string>(state->config->Get("Graphics.ShaderPack", std::string(""))));

    RegisterBuiltInPasses();
    CreatePasses();
    hizBuildPass.Create(shaders);
    CreateTimestampPool();

    Log::Info(std::string("[INFO] Render engine initialized (") + GetBackendName() + ", "
        + std::to_string(passes.size()) + " passes).");
}

VkDevice VulkanRenderEngine::GetVkDevice() const { return ::GetDevice(); }
VkPhysicalDevice VulkanRenderEngine::GetVkPhysicalDevice() const { return ::GetPhysicalDevice(); }
VmaAllocator VulkanRenderEngine::GetAllocator() const { return vmaAllocator; }
VkQueue VulkanRenderEngine::GetGraphicsQueue() const { return graphicsQueue; }
uint32_t VulkanRenderEngine::GetGraphicsQueueFamily() const { return graphicsQueueFamilyIndex; }
VkRenderPass VulkanRenderEngine::GetMainRenderPass() const { return ::renderPass; }
VkRenderPass VulkanRenderEngine::GetOverlayRenderPass() const { return ::overlayRenderPass; }
VkFormat VulkanRenderEngine::GetDepthFormat() const { return depthFormat; }
VkExtent2D VulkanRenderEngine::GetSwapchainExtent() const { return swapchainExtent; }
VkDescriptorSetLayout VulkanRenderEngine::GetCameraSetLayout() const { return cameraSetLayout; }
VkDescriptorSetLayout VulkanRenderEngine::GetTextureSetLayout() const { return textureSetLayout; }
VkDescriptorSet VulkanRenderEngine::GetCameraSet(uint32_t frameIndex) const { return cameraSets[frameIndex]; }
VkDescriptorSet VulkanRenderEngine::GetTextureSet() const { return textureSet; }

DepthTarget VulkanRenderEngine::CreateDepthTarget(uint32_t width, uint32_t height, bool compareSampler, uint32_t layers)
{
    DepthTarget target;
    VkDevice dev = ::GetDevice();
    layers = std::max(layers, 1u);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = depthFormat;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(vmaAllocator, &imageInfo, &allocInfo, &target.image, &target.allocation, nullptr) != VK_SUCCESS)
    {
        Log::Error("[ERROR] CreateDepthTarget: image allocation failed.");
        return DepthTarget{};
    }
    target.width = width;
    target.height = height;
    target.layers = layers;

    auto makeView = [&](VkImageView& view, VkImageViewType type, uint32_t firstLayer, uint32_t layerCount)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = target.image;
        viewInfo.viewType = type;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, firstLayer, layerCount };
        return vkCreateImageView(dev, &viewInfo, nullptr, &view) == VK_SUCCESS;
    };

    // The sampling view covers every layer; framebuffers need one view per layer.
    bool ok = makeView(target.view, layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, 0, layers);
    if (ok && layers > 1)
    {
        target.layerViews.assign(layers, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < layers && ok; i++) ok = makeView(target.layerViews[i], VK_IMAGE_VIEW_TYPE_2D, i, 1);
    }

    if (ok)
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // Outside the map is lit.
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.compareEnable = compareSampler ? VK_TRUE : VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        ok = vkCreateSampler(dev, &samplerInfo, nullptr, &target.sampler) == VK_SUCCESS;
    }

    if (ok)
    {
        // Depth only: cleared on load, stored, and left sampleable. The two
        // dependencies order writes against both the previous frame's reads
        // and this frame's later fragment-shader reads.
        VkAttachmentDescription depth{};
        depth.format = depthFormat;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference depthRef{ 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency dependencies[2]{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo passInfo{};
        passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        passInfo.attachmentCount = 1;
        passInfo.pAttachments = &depth;
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpass;
        passInfo.dependencyCount = 2;
        passInfo.pDependencies = dependencies;
        ok = vkCreateRenderPass(dev, &passInfo, nullptr, &target.renderPass) == VK_SUCCESS;
    }

    if (ok)
    {
        for (uint32_t i = 0; i < layers && ok; i++)
        {
            VkImageView attachment = layers > 1 ? target.layerViews[i] : target.view;
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = target.renderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &attachment;
            framebufferInfo.width = width;
            framebufferInfo.height = height;
            framebufferInfo.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            ok = vkCreateFramebuffer(dev, &framebufferInfo, nullptr, &framebuffer) == VK_SUCCESS;
            if (ok) target.framebuffers.push_back(framebuffer);
        }
    }

    if (!ok)
    {
        Log::Error("[ERROR] CreateDepthTarget: failed to build the target.");
        DestroyDepthTarget(target);
        return DepthTarget{};
    }
    return target;
}

void VulkanRenderEngine::DestroyDepthTarget(DepthTarget& target)
{
    VkDevice dev = ::GetDevice();
    if (target.image != VK_NULL_HANDLE || target.view != VK_NULL_HANDLE) WaitIdle();

    for (VkFramebuffer framebuffer : target.framebuffers) vkDestroyFramebuffer(dev, framebuffer, nullptr);
    if (target.renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(dev, target.renderPass, nullptr);
    if (target.sampler != VK_NULL_HANDLE) vkDestroySampler(dev, target.sampler, nullptr);
    for (VkImageView view : target.layerViews)
    {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(dev, view, nullptr);
    }
    if (target.view != VK_NULL_HANDLE) vkDestroyImageView(dev, target.view, nullptr);
    if (target.image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, target.image, target.allocation);
    target = DepthTarget{};
}

bool VulkanRenderEngine::CanReceiveShadows() const { return shadowedPipeline != VK_NULL_HANDLE; }
VkDescriptorSet VulkanRenderEngine::GetShadowSet() const { return shadowSet; }

void VulkanRenderEngine::SetShadowMap(const DepthTarget& map)
{
    if (!map.IsValid()) return;
    WaitIdle(); // The set may be bound by frames still in flight.

    VkDescriptorImageInfo imageInfo{ map.sampler, map.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = shadowSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(::GetDevice(), 1, &write, 0, nullptr);
}

void VulkanRenderEngine::SetShadows(const ShadowCascades& cascades)
{
    auto* base = static_cast<uint8_t*>(cameraUBOsMapped[currentFrame]);

    glm::mat4 matrices[ShadowCascades::MAX_CASCADES];
    glm::vec4 splits(0.0f), texels(0.0f), ranges(1.0f);
    for (uint32_t i = 0; i < ShadowCascades::MAX_CASCADES; i++)
    {
        // Unused cascades repeat the last one so a lookup never reads garbage.
        uint32_t source = std::min(i, cascades.count > 0 ? cascades.count - 1 : 0u);
        matrices[i] = cascades.viewProj[source];
        splits[static_cast<int>(i)] = cascades.splitFar[source];
        texels[static_cast<int>(i)] = cascades.worldTexel[source];
        ranges[static_cast<int>(i)] = std::max(cascades.range[source], 1.0f);
    }
    glm::vec4 params(cascades.darkness, cascades.mapTexel, 0.0f, cascades.debugView ? 1.0f : 0.0f);

    std::memcpy(base + offsetof(CameraUBO, shadowViewProj), matrices, sizeof(matrices));
    std::memcpy(base + offsetof(CameraUBO, shadowParams), &params, sizeof(params));
    std::memcpy(base + offsetof(CameraUBO, shadowSplits), &splits, sizeof(splits));
    std::memcpy(base + offsetof(CameraUBO, shadowTexels), &texels, sizeof(texels));
    std::memcpy(base + offsetof(CameraUBO, shadowRanges), &ranges, sizeof(ranges));
    shadowsActiveThisFrame = true;
}

void VulkanRenderEngine::CreateTimestampPool()
{
    state->timings.gpuMs = -1.0f; // "n/a" until a sample arrives

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(GetPhysicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(GetPhysicalDevice(), &familyCount, families.data());

    uint32_t validBits = graphicsQueueFamilyIndex < familyCount ? families[graphicsQueueFamilyIndex].timestampValidBits : 0;
    if (validBits == 0) return;
    timestampMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1);
    timestampMsPerTick = static_cast<double>(physicalDevice.properties.limits.timestampPeriod) / 1.0e6;

    VkQueryPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = 3 * 2;
    if (vkCreateQueryPool(GetDevice(), &poolInfo, nullptr, &timestampPool) != VK_SUCCESS) timestampPool = VK_NULL_HANDLE;
}

// Call after the slot's fence wait. GPU time is smoothed like the CPU timers.
void VulkanRenderEngine::ReadTimestamps(uint32_t slot)
{
    if (timestampPool == VK_NULL_HANDLE || !timestampPending[slot]) return;
    timestampPending[slot] = false;

    uint64_t ticks[2];
    if (vkGetQueryPoolResults(GetDevice(), timestampPool, slot * 2, 2, sizeof(ticks), ticks,
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;

    float ms = static_cast<float>(((ticks[1] - ticks[0]) & timestampMask) * timestampMsPerTick);
    float previous = state->timings.gpuMs.load();
    state->timings.gpuMs.store(previous < 0.0f ? ms : previous + (ms - previous) * 0.05f);
}

void VulkanRenderEngine::RegisterBuiltInPasses()
{
    AddPass(std::make_unique<Passes::ShadowPass>());
    AddPass(std::make_unique<Passes::SkyboxPass>());
    AddPass(std::make_unique<Passes::TerrainPass>());
    AddPass(std::make_unique<Passes::EntityPass>());
    AddPass(std::make_unique<Passes::ModeledBEPass>());
    AddPass(std::make_unique<Passes::ItemPass>());
    AddPass(std::make_unique<Passes::NonCubicPass>());
    AddPass(std::make_unique<Passes::ParticlePass>());
    AddPass(std::make_unique<Passes::CloudPass>());
    AddPass(std::make_unique<Passes::SelectionPass>());
    AddPass(std::make_unique<Passes::HandPass>());
    AddPass(std::make_unique<Passes::ShaderPackPass>());
    AddPass(std::make_unique<Passes::GUIPass>());
}

void VulkanRenderEngine::AttachTextures(const TextureManager& textures)
{
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = textures.array.view;
    imageInfo.sampler = textures.array.sampler;

    VkWriteDescriptorSet textureWrite{};
    textureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    textureWrite.dstSet = GetTextureSet();
    textureWrite.dstBinding = 0;
    textureWrite.descriptorCount = 1;
    textureWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureWrite.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(GetDevice(), 1, &textureWrite, 0, nullptr);
}

void VulkanRenderEngine::AttachGUI(const TextureManager* textures)
{
    GUIController::Init(state->window, GetRenderPass(), 2, state, textures); // 2 is the swapchain image count
    guiAttached = true;
}

void VulkanRenderEngine::StopRendering()
{
    // The last submitted frame's command buffer may still be executing on
    // the GPU (fences are only waited on at the *start* of the next frame,
    // which never comes once the frame loop has exited) and it references
    // ImGui's descriptor pool/pipeline and every pass's pipelines - all
    // destroyed outright below.
    WaitIdle();

    if (guiAttached)
    {
        GUIController::Shutdown();
        guiAttached = false;
    }
    hizBuildPass.Destroy();
    DestroyPasses();

    if (timestampPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(GetDevice(), timestampPool, nullptr);
        timestampPool = VK_NULL_HANDLE;
    }

    if (screenshotBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, screenshotBuffer.buffer, screenshotBuffer.allocation);
        screenshotBuffer = AllocatedBuffer{};
    }
}

void VulkanRenderEngine::Shutdown()
{
    // Normally already done by StopRendering(); harmless if so.
    if (passesCreated) StopRendering();

    ::Cleanup();
    initialized = false;
}

bool VulkanRenderEngine::BeginFrame()
{
    // Resize requests land here (set by VulkanInit's GLFW framebuffer-size
    // callback) - this is the only thread allowed to touch the swapchain,
    // so recreation happens here, between frames, rather than in the
    // callback itself.
    if (state->framebufferResized.exchange(false))
    {
        RecreateSwapchain(state->pendingFramebufferWidth.load(), state->pendingFramebufferHeight.load());
    }

    // Between frames is also the only safe point to rebuild pipelines.
    if (shaderReloadRequested.exchange(false))
    {
        ReloadPassShaders();
    }

    // Have the CPU wait until this frame slot's previous submission is done.
    auto waitStart = std::chrono::steady_clock::now();
    vkWaitForFences(GetDevice(), 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
    ReadTimestamps(currentFrame);

    VkResult result = vkAcquireNextImageKHR(
        GetDevice(),
        GetSwapchain(),
        UINT64_MAX,
        imageAvailableSemaphores[currentFrame],
        VK_NULL_HANDLE,
        &imageIndex
    );
    if (state->debugHudVisible)
    {
        state->timings.waitRawMs.store(
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - waitStart).count());
    }

    // The swapchain is stale (surface no longer matches - a resize/fullscreen
    // toggle that outran the framebuffer-size callback, or a minimized
    // window). Skip this frame; RecreateSwapchain runs at the top of the
    // next BeginFrame now that a resize is flagged.
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
    {
        state->framebufferResized = true;
        return false;
    }

    // Suboptimal still yields a presentable image; only VK_SUCCESS and
    // VK_SUBOPTIMAL_KHR are non-fatal per the Vulkan spec.
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        throw std::runtime_error("[ERROR] Failed to acquire swapchain image!");
    }

    // Only reset once we know this slot will actually be submitted -
    // resetting before a skipped frame would leave the next wait hanging.
    vkResetFences(GetDevice(), 1, &inFlightFences[currentFrame]);
    return true;
}

void VulkanRenderEngine::SubmitFrame(const SceneView& scene)
{
    VkCommandBuffer cmd = commandBuffers[currentFrame];

    // Debug HUD data is only gathered while it's visible: hand over last
    // frame's occlusion counts (BeginFrame below resets them), then tell this
    // frame's culling whether to count.
    const bool profile = state->debugHudVisible;
    if (profile)
    {
        state->sectionsTested = occlusionSnapshot.tested;
        state->sectionsOccluded = occlusionSnapshot.occluded;
    }
    occlusionSnapshot.collectStats = profile;

    // This slot's fence was waited in BeginFrame, so its Hi-Z readback from
    // a couple of frames ago is done - publish it before the chunk passes
    // cull against it.
    hizBuildPass.BeginFrame(currentFrame);
    // Past this slot's fence wait: lets SlabBuffer reuse regions freed
    // enough frames ago (see SlabBuffer::FREE_DELAY_FRAMES).
    renderFrameCounter++;

    BeginCommandBuffer(cmd);
    if (profile && timestampPool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(cmd, timestampPool, currentFrame * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool, currentFrame * 2);
    }
    UploadCamera(scene);
    shadowsActiveThisFrame = false;

    FrameContext frame;
    frame.engine = this;
    frame.state = state;
    frame.scene = &scene;
    frame.frameIndex = currentFrame;
    frame.extent = GetExtent();
    frame.commandBuffer = cmd;
    frame.viewProj = scene.proj * scene.view;

    // Pre-main passes (shadow maps, ...) record outside any render pass.
    RecordPasses(frame, PassStage::PRE_MAIN);

    BeginMainPass(cmd, scene);
    RecordPasses(frame, PassStage::MAIN);
    EndMainPass(cmd);

    // Overlays draw over the finished frame in a pass that keeps it. Called
    // even with none registered: it also closes the frame's debug counters.
    bool overlays = HasEnabledPass(PassStage::OVERLAY);
    if (overlays) BeginOverlayPass(cmd);
    RecordPasses(frame, PassStage::OVERLAY);
    if (overlays) vkCmdEndRenderPass(cmd);

    bool capture = screenshotRequested.exchange(false);
    if (capture) RecordScreenshotCopy(cmd);
    // Outside the render pass instance now (compute can't run inside one) -
    // see HiZBuildPass's own comment on why it isn't a normal RenderPass.
    hizBuildPass.Record(cmd, currentFrame, frame.viewProj);
    if (profile && timestampPool != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool, currentFrame * 2 + 1);
        timestampPending[currentFrame] = true;
    }
    vkEndCommandBuffer(cmd);
    SubmitCommandBuffer(cmd);
    PresentImage();
    NotifyFramePresented();
    if (capture) FinishScreenshot();

    currentFrame = (currentFrame + 1) % maxFramesInFlight;
}

RenderExtent VulkanRenderEngine::GetExtent() const
{
    return RenderExtent{ swapchainExtent.width, swapchainExtent.height };
}

void VulkanRenderEngine::WaitIdle()
{
    if (initialized) vkDeviceWaitIdle(GetDevice());
}

void VulkanRenderEngine::ToggleWindowMode()
{
    ::ToggleWindowMode();
}

void VulkanRenderEngine::ApplyWindowSettings()
{
    ::ApplyWindowSettings();
}

void VulkanRenderEngine::BeginCommandBuffer(VkCommandBuffer cmd)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);
}

void VulkanRenderEngine::UploadCamera(const SceneView& scene)
{
    CameraUBO ubo{ scene.view, scene.proj, scene.lightingEnabled ? 1.0f : 0.0f,
        scene.brightnessFactor, scene.dayBrightness, scene.lightFalloff,
        scene.shadeTop, scene.shadeBottom, scene.shadeNorthSouth, scene.shadeEastWest,
        glm::vec4(scene.skyColor, 1.0f), glm::vec4(scene.cameraPosition, 1.0f), scene.fogStart, scene.fogEnd,
        scene.specularStrength, scene.time, glm::vec4(scene.sunDirection, 0.0f) };
    std::memcpy(cameraUBOsMapped[currentFrame], &ubo, sizeof(ubo));
}

void VulkanRenderEngine::BeginMainPass(VkCommandBuffer cmd, const SceneView& scene)
{
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{scene.skyColor.r, scene.skyColor.g, scene.skyColor.b, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = framebuffers[imageIndex];
    renderPassInfo.renderArea.extent = swapchainExtent;
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Every pipeline uses dynamic viewport/scissor - set once, here, for
    // all passes.
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainExtent.width);
    viewport.height = static_cast<float>(swapchainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void VulkanRenderEngine::BeginOverlayPass(VkCommandBuffer cmd)
{
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = overlayRenderPass;
    renderPassInfo.framebuffer = framebuffers[imageIndex];
    renderPassInfo.renderArea.extent = swapchainExtent;
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(swapchainExtent.width),
        static_cast<float>(swapchainExtent.height), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { 0, 0 }, swapchainExtent };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void VulkanRenderEngine::EndMainPass(VkCommandBuffer cmd)
{
    vkCmdEndRenderPass(cmd);
    // vkEndCommandBuffer is NOT here: SubmitFrame still records HiZBuildPass
    // after the render pass ends - see its own vkEndCommandBuffer call.
}

void VulkanRenderEngine::RecordScreenshotCopy(VkCommandBuffer cmd)
{
    VkDeviceSize size = static_cast<VkDeviceSize>(swapchainExtent.width) * swapchainExtent.height * 4;
    if (screenshotBuffer.buffer != VK_NULL_HANDLE && screenshotBuffer.info.size < size)
    {
        vmaDestroyBuffer(vmaAllocator, screenshotBuffer.buffer, screenshotBuffer.allocation);
        screenshotBuffer = AllocatedBuffer{};
    }
    if (screenshotBuffer.buffer == VK_NULL_HANDLE)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &screenshotBuffer.buffer,
                &screenshotBuffer.allocation, &screenshotBuffer.info) != VK_SUCCESS)
        {
            Log::Error("Screenshot failed: couldn't allocate a readback buffer.");
            screenshotBuffer = AllocatedBuffer{};
            return;
        }
    }
    screenshotExtent = swapchainExtent;

    std::vector<VkImage> images = swapchain.get_images().value();
    VkImage image = images[imageIndex];

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { swapchainExtent.width, swapchainExtent.height, 1 };
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screenshotBuffer.buffer, 1, &region);

    // Back to present, and make the copy visible to the host read.
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &hostBarrier, 0, nullptr, 0, nullptr);
}

void VulkanRenderEngine::FinishScreenshot()
{
    if (screenshotBuffer.buffer == VK_NULL_HANDLE) return;

    // A one-frame stall on F2 is fine.
    vkWaitForFences(GetDevice(), 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
    vmaInvalidateAllocation(vmaAllocator, screenshotBuffer.allocation, 0, VK_WHOLE_SIZE);

    size_t pixelCount = static_cast<size_t>(screenshotExtent.width) * screenshotExtent.height;
    std::vector<uint8_t> rgba(pixelCount * 4);
    std::memcpy(rgba.data(), screenshotBuffer.info.pMappedData, rgba.size());

    // Most Windows swapchains are BGRA; PNG wants RGBA. Alpha is forced
    // opaque (the swapchain's alpha isn't meaningful).
    VkFormat format = swapchain.image_format;
    bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < pixelCount; i++)
    {
        if (bgra) std::swap(rgba[i * 4], rgba[i * 4 + 2]);
        rgba[i * 4 + 3] = 255;
    }

    Screenshot::SaveAsync(std::move(rgba), screenshotExtent.width, screenshotExtent.height,
        state->config->ResolvePath("screenshots"));
}

void VulkanRenderEngine::SubmitCommandBuffer(VkCommandBuffer cmd)
{
    // Wait for the acquired image before writing color; signal
    // renderFinished for the present, and this slot's fence for the CPU.
    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[currentFrame]};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to submit semaphore to graphics queue.");
    }
}

void VulkanRenderEngine::PresentImage()
{
    VkSemaphore waitSemaphores[] = {renderFinishedSemaphores[currentFrame]};
    VkSwapchainKHR swapchains[] = {GetSwapchain()};

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = waitSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    VkResult result = vkQueuePresentKHR(presentQueue, &presentInfo);
    // Losing exclusive fullscreen (alt-tab, etc.) just means rebuilding.
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR
        || result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
    {
        state->framebufferResized = true;
    }
    else if (result != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to present swapchain image!");
    }
}

} // namespace Volcano::Engine::Vulkan
