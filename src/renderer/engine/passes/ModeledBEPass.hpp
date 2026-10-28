#pragma once
#ifndef VOLCANO_ENGINE_PASSES_MODELED_BE_PASS_H
#define VOLCANO_ENGINE_PASSES_MODELED_BE_PASS_H

#include <vector>
#include "../RenderPass.hpp"
#include "../../blockentity/ModeledBERenderer.hpp"
#include "../vulkan/VulkanRenderEngine.hpp"

namespace Volcano::Engine::Passes {

// Modeled block entities: blocks drawn as an entity model rather than chunk
// geometry (chests so far). Draws through EntityRenderer's textured pipeline,
// so it must come after EntityPass (which creates it). Everything visible is
// grouped by model and skin and drawn instanced, through one indirect draw
// per group when Graphics.IndirectDraw is on.
class ModeledBEPass : public RenderPass {
public:
    static constexpr const char* NAME = "ModeledBlockEntities";

    ModeledBEPass() : RenderPass(NAME) {}

    void Create(const ShaderLibrary& /*shaders*/) override {}

    void ReloadShaders(const ShaderLibrary& /*shaders*/) override {}

    void Record(FrameContext& frame) override
    {
        if (!started)
        {
            blockEntities.Start(&frame.state->world);
            started = true;
        }
        blockEntities.SetCameraPosition(frame.scene->cameraPosition);
        blockEntities.CopyDraws(draws);
        uint64_t triangles = EntityRenderer::RecordBlockEntities(frame.commandBuffer, frame.frameIndex, draws,
            frame.scene->proj * frame.scene->view, frame.scene->indirectDraw, frame.engine->GetVulkan());
        if (frame.state->debugHudVisible) frame.state->trianglesDrawn[GlobalState::TRIANGLES_ENTITY] += triangles;
    }

    void Destroy() override
    {
        blockEntities.Stop();
        started = false;
    }

private:
    ModeledBERenderer blockEntities;
    std::vector<EntityRenderer::ModeledBEDraw> draws;
    bool started = false;
};

} // namespace Volcano::Engine::Passes

#endif
