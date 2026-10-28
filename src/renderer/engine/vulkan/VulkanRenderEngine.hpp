#pragma once
#ifndef VOLCANO_ENGINE_VULKAN_RENDER_ENGINE_H
#define VOLCANO_ENGINE_VULKAN_RENDER_ENGINE_H

#include <atomic>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "../RenderEngine.hpp"
#include "../../models/AllocatedBuffer.hpp"
#include "../passes/HiZBuildPass.hpp"

namespace Volcano::Engine::Vulkan {

// A depth-only render target a pass renders into and later samples (shadow
// maps, depth pre-passes, ...). Made by VulkanRenderEngine::CreateDepthTarget,
// freed by DestroyDepthTarget. After its render pass ends, the image is in
// SHADER_READ_ONLY_OPTIMAL, ready to bind through `view` + `sampler`.
struct DepthTarget {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;        // All layers (a 2D array view when layers > 1), for sampling.
    VkSampler sampler = VK_NULL_HANDLE;       // Hardware depth-compare (LESS_OR_EQUAL) when requested; white border.
    VkRenderPass renderPass = VK_NULL_HANDLE; // Depth only: clear on load, store, ends sampleable.
    std::vector<VkImageView> layerViews;      // One 2D view per layer (empty for a single layer).
    std::vector<VkFramebuffer> framebuffers;  // One per layer: render into framebuffers[i].
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 1;

    bool IsValid() const { return !framebuffers.empty(); }
};

// What the shadow receivers need, per frame: up to three cascades of the sun's
// depth map (index 0 is the tightest, nearest to the camera).
struct ShadowCascades {
    static constexpr uint32_t MAX_CASCADES = 3;
    uint32_t count = 0;
    glm::mat4 viewProj[MAX_CASCADES] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
    float splitFar[MAX_CASCADES] = {};    // View distance where each cascade ends.
    float worldTexel[MAX_CASCADES] = {};  // Blocks per map texel.
    float range[MAX_CASCADES] = {};       // Depth span of each light matrix, in blocks.
    float darkness = 0.0f;                // How much shadow dims sky light, 0-1.
    float mapTexel = 0.0f;                // One map texel in uv.
    bool debugView = false;               // Magenta where shadowed, blue outside the map.
};

// The Vulkan backend: wraps VulkanInit's device/swapchain globals and owns
// the per-frame synchronization (fence wait, image acquire, submit,
// present) that used to live in RenderThread. A frame is one command
// buffer and one render pass (color + depth, cleared to the sky color);
// every RenderPass in the engine's list records into it, in order.
class VulkanRenderEngine : public RenderEngine {
public:
    explicit VulkanRenderEngine(GlobalState* globalState) : RenderEngine(globalState) {}

    void Init() override;
    void AttachTextures(const TextureManager& textures) override;
    void AttachGUI(const TextureManager* textures) override;
    void StopRendering() override;
    void Shutdown() override;
    const char* GetBackendName() const override { return "Vulkan"; }
    VulkanRenderEngine* GetVulkan() override { return this; }

    // ---- Handles and factories for passes and mods ----
    // Everything a pass needs to build and record its own Vulkan work. Handles
    // stay owned by the engine; don't destroy them. Pipelines made against
    // GetMainRenderPass() also work in the overlay render pass.

    VkDevice GetVkDevice() const;
    VkPhysicalDevice GetVkPhysicalDevice() const;
    VmaAllocator GetAllocator() const;
    VkQueue GetGraphicsQueue() const;
    uint32_t GetGraphicsQueueFamily() const;
    VkRenderPass GetMainRenderPass() const;
    VkRenderPass GetOverlayRenderPass() const;
    VkFormat GetDepthFormat() const;
    VkExtent2D GetSwapchainExtent() const;

    // The world descriptor sets the chunk/entity pipelines use: the camera UBO
    // (set 0, one per frame in flight) and the block texture array (set 1).
    VkDescriptorSetLayout GetCameraSetLayout() const;
    VkDescriptorSetLayout GetTextureSetLayout() const;
    VkDescriptorSet GetCameraSet(uint32_t frameIndex) const;
    VkDescriptorSet GetTextureSet() const;

    // Creates a width x height depth target of `layers` array layers (see
    // DepthTarget). `compareSampler` makes its sampler a shadow sampler. Returns
    // an invalid target on failure.
    DepthTarget CreateDepthTarget(uint32_t width, uint32_t height, bool compareSampler, uint32_t layers = 1);
    // Waits for the GPU, then frees the target and resets it.
    void DestroyDepthTarget(DepthTarget& target);

    // ---- Shadow receiving ----
    // The terrain pass draws with its shadow-receiving pipelines in any frame
    // where a shadow pass called SetShadows before the main pass.

    // Points the shadow descriptor set (set 2 of the shadowed pipelines) at a
    // map. Waits for the GPU, so call it only when the map is (re)created.
    void SetShadowMap(const DepthTarget& map);

    // Uploads this frame's shadow transform into the camera UBO and marks the
    // frame as having shadows. `darkness` (0-1) is how much shadow dims sky
    // light, `texel` one map texel in uv, `bias` the depth bias. Call every
    // frame the map should be used, from a PRE_MAIN pass.
    // Uploads this frame's shadow cascades into the camera UBO and marks the
    // frame as having shadows. Call every frame the map should be used, from a
    // PRE_MAIN pass.
    void SetShadows(const ShadowCascades& cascades);

    // Whether SetShadows was called this frame, and whether the
    // shadowed terrain pipelines exist (they don't when a shader pack
    // replaces the terrain shaders).
    bool AreShadowsActive() const { return shadowsActiveThisFrame; }
    bool CanReceiveShadows() const;
    VkDescriptorSet GetShadowSet() const;

    bool BeginFrame() override;
    void SubmitFrame(const SceneView& scene) override;
    RenderExtent GetExtent() const override;
    uint32_t GetFramesInFlight() const override { return maxFramesInFlight; }
    void WaitIdle() override;

    void ToggleWindowMode() override;
    void ApplyWindowSettings() override;
    void RequestScreenshot() override { screenshotRequested = true; }

private:
    // GPU frame time: two timestamps per frame slot (start/end of its
    // command buffer), read back once that slot's fence has signaled.
    VkQueryPool timestampPool = VK_NULL_HANDLE;
    bool timestampPending[3]{};
    double timestampMsPerTick = 0.0;
    uint64_t timestampMask = 0;
    void CreateTimestampPool();
    void ReadTimestamps(uint32_t slot);

    // F2 capture: the swapchain image is copied into a host buffer after the
    // main pass, then read back once that frame's fence signals.
    std::atomic<bool> screenshotRequested{false};
    AllocatedBuffer screenshotBuffer{};
    VkExtent2D screenshotExtent{};
    void RecordScreenshotCopy(VkCommandBuffer cmd);
    void FinishScreenshot();

    bool shadowsActiveThisFrame = false;
    uint32_t currentFrame = 0;
    uint32_t imageIndex = 0;
    uint32_t maxFramesInFlight = 2;
    bool initialized = false;
    bool guiAttached = false;

    // Not one of RenderEngine's own passes
    Passes::HiZBuildPass hizBuildPass;

    // Built-in passes, in draw order
    void RegisterBuiltInPasses();

    // SubmitFrame(), step by step.
    void BeginCommandBuffer(VkCommandBuffer cmd);
    void UploadCamera(const SceneView& scene);
    void BeginMainPass(VkCommandBuffer cmd, const SceneView& scene);
    void EndMainPass(VkCommandBuffer cmd);
    void BeginOverlayPass(VkCommandBuffer cmd);
    void SubmitCommandBuffer(VkCommandBuffer cmd);
    void PresentImage();
};

} // namespace Volcano::Engine::Vulkan

#endif
