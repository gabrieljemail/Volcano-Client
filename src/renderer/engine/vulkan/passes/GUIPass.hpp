#pragma once
#ifndef VOLCANO_ENGINE_PASSES_GUI_PASS_H
#define VOLCANO_ENGINE_PASSES_GUI_PASS_H

#include "../RenderPass.hpp"
#include "../../gui/GUIController.hpp"

namespace Volcano::Engine::Passes {

// ImGui draw data (HUD, chat, screens, debug overlay)
class GUIPass : public RenderPass {
public:
    static constexpr const char* NAME = "GUI";

    GUIPass() : RenderPass(NAME) {}

    // ImGui's pipeline isn't built from the ShaderLibrary.
    void ReloadShaders(const ShaderLibrary& /*shaders*/) override {}

    void Record(FrameContext& frame) override
    {
        GUIController::Render(frame.commandBuffer, frame.scene);
    }
};

} // namespace Volcano::Engine::Passes

#endif
