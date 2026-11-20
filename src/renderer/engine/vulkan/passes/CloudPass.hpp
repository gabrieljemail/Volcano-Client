#pragma once
#ifndef VOLCANO_ENGINE_PASSES_CLOUD_PASS_H
#define VOLCANO_ENGINE_PASSES_CLOUD_PASS_H

#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "../../RenderPass.hpp"
#include "../../../misc/CloudScene.hpp"

namespace Volcano::Engine::Passes {

// Vanilla's clouds: the cells of textures/environment/clouds.png laid out in
// 12-block squares, drifting slowly, 4 blocks thick (Fancy) or a single flat
// layer (Fast). The grid around the camera is rebuilt on the CPU whenever the
// camera crosses a cell, camera-relative so float precision holds anywhere.
// Drawn after the opaque passes, blended, depth-tested but not depth-writing
// (a cloud must not occlude entities in the Hi-Z pyramid).
// Settings: Graphics.Clouds (0 off, 1 fast, 2 fancy), Graphics.CloudHeight.
class CloudPass : public RenderPass {
public:
    static constexpr const char* NAME = "Clouds";
    static constexpr const char* VERTEX_SHADER = "clouds.vert";
    static constexpr const char* FRAGMENT_SHADER = "clouds.frag";

    CloudPass() : RenderPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;
    void ReloadShaders(const ShaderLibrary& shaders) override;
    void ReloadTextures(const ShaderLibrary& shaders) override;

private:
    static constexpr size_t FRAME_SLOTS = 4;

    // One mesh per frame in flight, each remembering which grid it holds.
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        CloudScene::Vertex* mapped = nullptr;
        uint32_t vertexCount = 0;
        int centerX = 0, centerZ = 0;
        int mode = -1; // 1 fast, 2 fancy; -1 = never built
    };

    CloudScene scene; // The cells, the grid mesh and the per-frame placement, shared with Direct3D 11.
    std::array<Slot, FRAME_SLOTS> slots;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    void CreatePipeline(const ShaderLibrary& shaders);
    void DestroyPipeline();
};

} // namespace Volcano::Engine::Passes

#endif
