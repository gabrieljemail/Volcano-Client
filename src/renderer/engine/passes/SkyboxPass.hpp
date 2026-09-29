#pragma once
#ifndef VOLCANO_ENGINE_PASSES_SKYBOX_PASS_H
#define VOLCANO_ENGINE_PASSES_SKYBOX_PASS_H

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "../RenderPass.hpp"

namespace Volcano::Engine::Passes {

// The slowly rotating title-screen panorama — vanilla's CubeMap, drawn from
// the six panorama_N.png faces under assets/minecraft/textures/gui/title/
// background/. Runs first so everything else draws over it.
//
// Only draws while SceneView::menuBackground is set (the client isn't in a
// play state yet / no chunks are loaded); in-game the sky stays the plain
// clear color. If the faces can't be loaded the pass builds nothing and
// Record() is a no-op, so the menu just shows the clear color.
class SkyboxPass : public RenderPass {
public:
    static constexpr const char* NAME = "Skybox";
    static constexpr const char* VERTEX_SHADER = "skybox.vert";
    static constexpr const char* FRAGMENT_SHADER = "skybox.frag";

    SkyboxPass() : RenderPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;

    // Only the pipeline depends on shaders; the cubemap survives a reload.
    void ReloadShaders(const ShaderLibrary& shaders) override;

private:
    VkImage cubeImage = VK_NULL_HANDLE;
    VmaAllocation cubeAllocation = VK_NULL_HANDLE;
    VkImageView cubeView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    bool LoadCubemap();
    void CreateDescriptors();
    void CreatePipeline(const ShaderLibrary& shaders);
    void DestroyPipeline();
};

} // namespace Volcano::Engine::Passes

#endif
