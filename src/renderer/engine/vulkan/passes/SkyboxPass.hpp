#pragma once
#ifndef VOLCANO_ENGINE_PASSES_SKYBOX_PASS_H
#define VOLCANO_ENGINE_PASSES_SKYBOX_PASS_H

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "../RenderPass.hpp"

namespace Volcano::Engine::Passes {

// The background cube, drawn first so everything else draws over it:
// - in menus (SceneView::menuBackground), the slowly rotating title
//   panorama
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

    // Reloads the panorama and sun/moon art (full rebuild; GPU is idle).
    void ReloadTextures(const ShaderLibrary& shaders) override;

private:
    VkImage cubeImage = VK_NULL_HANDLE;
    VmaAllocation cubeAllocation = VK_NULL_HANDLE;
    VkImageView cubeView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    // Sun (layer 0) and the 8 moon phases (layers 1-8), all 32x32.
    VkImage celestialImage = VK_NULL_HANDLE;
    VmaAllocation celestialAllocation = VK_NULL_HANDLE;
    VkImageView celestialView = VK_NULL_HANDLE;
    VkSampler celestialSampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    bool LoadCubemap();
    bool LoadCelestial();
    // Uploads `layers` RGBA8 images of size x size into a new 2D array (or
    // cube, if cube) image + view.
    bool UploadLayers(const std::vector<const uint8_t*>& layers, uint32_t size, bool cube,
                      VkImage& image, VmaAllocation& allocation, VkImageView& view);
    void CreateDescriptors();
    void CreatePipeline(const ShaderLibrary& shaders);
    void DestroyPipeline();
};

} // namespace Volcano::Engine::Passes

#endif
