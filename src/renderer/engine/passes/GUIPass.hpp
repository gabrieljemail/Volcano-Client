#pragma once
#ifndef VOLCANO_ENGINE_PASSES_GUI_PASS_H
#define VOLCANO_ENGINE_PASSES_GUI_PASS_H

#include "../RenderPass.hpp"
#include "../../gui/GUIController.hpp"

namespace Volcano::Engine::Passes {

// ImGui draw data (HUD, chat, screens, debug overlay) — always last so it
// sits on top of everything, including a shader pack overlay. The ImGui
// backend's own lifetime is the engine's (RenderEngine::AttachGUI /
// StopRendering), not this pass's: GUIController needs the texture manager
// that only exists after startup loading, well after passes are created.
class GUIPass : public RenderPass {
public:
    static constexpr const char* NAME = "GUI";

    GUIPass() : RenderPass(NAME) {}

    // ImGui's pipeline isn't built from the ShaderLibrary.
    void ReloadShaders(const ShaderLibrary& /*shaders*/) override {}

    void Record(FrameContext& frame) override
    {
        GUIController::Render(frame.commandBuffer);
    }
};

} // namespace Volcano::Engine::Passes

#endif
