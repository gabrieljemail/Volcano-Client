#pragma once
#ifndef VOLCANO_ENGINE_PASSES_PARTICLE_PASS_H
#define VOLCANO_ENGINE_PASSES_PARTICLE_PASS_H

#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "../RenderPass.hpp"
#include "../../particle/ParticleSystem.hpp"

namespace Volcano::Engine::Passes {

// Draws GlobalState::particles: every particle is one instance of a camera-facing quad, sampled from an atlas of
// the particle textures (see ParticleSprites). Blended and depth-tested but not depth-writing, so particles never
// hide things in the Hi-Z pyramid. Runs the particles' ticks as well, since it is where the frame's time passes.
// Setting: Graphics.Particles (0 all, 1 decreased, 2 minimal).
class ParticlePass : public RenderPass {
public:
    static constexpr const char* NAME = "Particles";
    static constexpr const char* VERTEX_SHADER = "particle.vert";
    static constexpr const char* FRAGMENT_SHADER = "particle.frag";

    ParticlePass() : RenderPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;
    void ReloadShaders(const ShaderLibrary& shaders) override;
    void ReloadTextures(const ShaderLibrary& shaders) override;

private:
    static constexpr size_t FRAME_SLOTS = 4;

    // One instance buffer per frame in flight.
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        ParticleInstance* mapped = nullptr;
    };

    std::array<Slot, FRAME_SLOTS> slots;
    VkImage atlasImage = VK_NULL_HANDLE;
    VmaAllocation atlasAllocation = VK_NULL_HANDLE;
    VkImageView atlasView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool atlasReady = false;
    bool atlasFailed = false;

    void CreateDescriptors();
    void CreatePipeline(const ShaderLibrary& shaders);
    void DestroyPipeline();
    bool UploadAtlas(const ParticleSprites& sprites);
    void DestroyAtlas();
};

} // namespace Volcano::Engine::Passes

#endif
