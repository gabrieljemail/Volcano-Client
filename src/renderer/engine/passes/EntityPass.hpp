#pragma once
#ifndef VOLCANO_ENGINE_PASSES_ENTITY_PASS_H
#define VOLCANO_ENGINE_PASSES_ENTITY_PASS_H

#include "../RenderPass.hpp"
#include "../../entity/EntityRenderer.hpp"

namespace Volcano::Engine::Passes {

// GlobalState::entities — the batched placeholder-cube draw plus the
// per-entity skinned humanoid draw. A thin adapter: EntityRenderer keeps
// doing the work, this just ties its Init/ReloadPipelines/Shutdown to the
// pass lifecycle. Shares the terrain pass's depth attachment, so draw
// order relative to it doesn't affect occlusion — see EntityRenderer's
// header. Shaders: entity.* and entity_textured.*.
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
            frame.scene->view, frame.scene->proj, frame.scene->cameraPosition);
    }

    void Destroy() override
    {
        EntityRenderer::Shutdown();
    }
};

} // namespace Volcano::Engine::Passes

#endif
