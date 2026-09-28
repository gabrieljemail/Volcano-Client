#pragma once
#ifndef VOLCANO_ENGINE_VULKAN_RENDER_ENGINE_H
#define VOLCANO_ENGINE_VULKAN_RENDER_ENGINE_H

#include <cstdint>
#include "../RenderEngine.hpp"

namespace Volcano::Engine::Vulkan {

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

    bool BeginFrame() override;
    void SubmitFrame(const SceneView& scene) override;
    RenderExtent GetExtent() const override;
    uint32_t GetFramesInFlight() const override { return maxFramesInFlight; }
    void WaitIdle() override;

    void ToggleWindowMode() override;

private:
    uint32_t currentFrame = 0;
    uint32_t imageIndex = 0;
    uint32_t maxFramesInFlight = 2;
    bool initialized = false;
    bool guiAttached = false;

    // Built-in passes, in draw order — see RenderEngine::AddPass for names.
    void RegisterBuiltInPasses();

    // SubmitFrame(), step by step.
    void BeginCommandBuffer(VkCommandBuffer cmd);
    void UploadCamera(const SceneView& scene);
    void BeginMainPass(VkCommandBuffer cmd, const SceneView& scene);
    void EndMainPass(VkCommandBuffer cmd);
    void SubmitCommandBuffer(VkCommandBuffer cmd);
    void PresentImage();
};

} // namespace Volcano::Engine::Vulkan

#endif
