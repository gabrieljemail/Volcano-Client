#pragma once
#ifndef VOLCANO_ENGINE_PASSES_CLOUD_PASS_H
#define VOLCANO_ENGINE_PASSES_CLOUD_PASS_H

#include <array>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "../RenderPass.hpp"

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
    static constexpr int TEXTURE_SIZE = 256;
    static constexpr int RADIUS_CELLS = 28;
    static constexpr float CELL_SIZE = 12.0f;
    static constexpr float THICKNESS = 4.0f;
    static constexpr float DRIFT_BLOCKS_PER_SECOND = 0.6f;
    static constexpr uint32_t MAX_VERTICES = 6u * 6u * (2 * RADIUS_CELLS) * (2 * RADIUS_CELLS);
    static constexpr size_t FRAME_SLOTS = 4;

    struct Vertex {
        float x, y, z;
        float shade;
    };

    // One mesh per frame in flight, each remembering which grid it holds.
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        Vertex* mapped = nullptr;
        uint32_t vertexCount = 0;
        int centerX = 0, centerZ = 0;
        int mode = -1; // 1 fast, 2 fancy; -1 = never built
    };

    std::vector<bool> cells; // TEXTURE_SIZE^2, true where a cloud is
    std::array<Slot, FRAME_SLOTS> slots;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    bool LoadCells();
    bool CellAt(int x, int z) const;
    void BuildMesh(Slot& slot, int centerX, int centerZ, int mode);
    void CreatePipeline(const ShaderLibrary& shaders);
    void DestroyPipeline();
};

} // namespace Volcano::Engine::Passes

#endif
