#pragma once
#ifndef VOLCANO_ENGINE_PASSES_ENTITY_PASS_H
#define VOLCANO_ENGINE_PASSES_ENTITY_PASS_H

#include "../RenderPass.hpp"
#include "../../entity/EntityRenderer.hpp"

namespace Volcano::Engine::Passes {

// GlobalState::entities
class EntityPass : public RenderPass {
public:
    static constexpr const char* NAME = "Entities";

    EntityPass() : RenderPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override
    {
        EntityRenderer::Init(shaders);
    }

    void ReloadShaders(const ShaderLibrary& shaders) override
    {
        EntityRenderer::ReloadPipelines(shaders);
    }

    void Record(FrameContext& frame) override
    {
        EntityRenderer::RecordDraw(frame.commandBuffer, frame.frameIndex, frame.state,
            frame.scene->view, frame.scene->proj, frame.scene->cameraPosition, frame.scene->indirectDraw);
    }

    void Destroy() override
    {
        EntityRenderer::Shutdown();
    }
};

} // namespace Volcano::Engine::Passes

#endif
