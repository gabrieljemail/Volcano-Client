#pragma once
#ifndef VOLCANO_ENGINE_PASSES_SELECTION_PASS_H
#define VOLCANO_ENGINE_PASSES_SELECTION_PASS_H

#include "../RenderPass.hpp"
#include "../../misc/SelectionRenderer.hpp"

namespace Volcano::Engine::Passes {

// Block selection outline/fill — after opaque/entity/non-cubic so it
// blends against whatever's actually there (including translucent surfaces
// like glass), and depth-tests against a wall correctly hiding it. Adapter
// around SelectionRenderer, same shape as EntityPass. Shaders: selection.*.
class SelectionPass : public RenderPass {
public:
    static constexpr const char* NAME = "Selection";

    SelectionPass() : RenderPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override
    {
        SelectionRenderer::Init(shaders);
    }

    void ReloadShaders(const ShaderLibrary& shaders) override
    {
        SelectionRenderer::ReloadPipelines(shaders);
    }

    void Record(FrameContext& frame) override
    {
        SelectionRenderer::RecordDraw(frame.commandBuffer, frame.frameIndex, frame.state);
    }

    void Destroy() override
    {
        SelectionRenderer::Shutdown();
    }
};

} // namespace Volcano::Engine::Passes

#endif
