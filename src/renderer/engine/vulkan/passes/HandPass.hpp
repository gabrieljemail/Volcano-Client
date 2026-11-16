#pragma once
#ifndef VOLCANO_ENGINE_PASSES_HAND_PASS_H
#define VOLCANO_ENGINE_PASSES_HAND_PASS_H

#include "../RenderPass.hpp"
#include "../RenderEngine.hpp"
#include "../../misc/HandModel.hpp"
#include "../../misc/ItemRenderer.hpp"
#include "../../../GlobalState.hpp"

namespace Volcano::Engine::Passes {

// The first-person arm and held items, drawn over everything else in the
// world (see HandModel). Must come after EntityPass: it borrows that pass's
// default skin. Shaders: hand.* and item.*.
class HandPass : public RenderPass {
public:
    static constexpr const char* NAME = "Hand";

    HandPass() : RenderPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override
    {
        hand.Create(shaders);
    }

    void Record(FrameContext& frame) override
    {
        if (frame.state->itemRenderer != nullptr)
        {
            frame.state->itemRenderer->EnsureWorldPipeline(frame.engine->GetShaderLibrary());
        }
        hand.Record(frame);
    }

    void Destroy() override
    {
        hand.Destroy();
    }

private:
    HandModel hand;
};

} // namespace Volcano::Engine::Passes

#endif
