#pragma once
#ifndef VOLCANO_GUI_IMGUI_INPUT_H
#define VOLCANO_GUI_IMGUI_INPUT_H

#include <Pincel/Pincel.hpp>

namespace Volcano::GUI {

// Hands Pincel a frame's mouse and keyboard, taken from ImGui, which already
// collects both from the window. Call after ImGui's NewFrame.
class ImGuiInput {
public:
    // `scale` is the GUI scale: ImGui's coordinates are in UI units, Pincel's in pixels.
    void Feed(Pincel::Canvas& canvas, float scale);

    // What was last fed, in pixels.
    const Pincel::MouseInput& Mouse() const { return mouse; }

private:
    Pincel::MouseInput mouse;
};

} // namespace Volcano::GUI

#endif
