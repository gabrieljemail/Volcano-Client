#pragma once
#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "GUIComponent.hpp"

namespace Volcano::GUI {

// Which screen edge/corner a GUIWindow is pinned to. The window's on-screen
// position is resolved fresh every frame from the *current* display size
// (see GUIController::ResolveAnchor) plus a pixel offset
enum class ScreenAnchor : uint8_t {
    TOP_LEFT = 0,
    TOP_RIGHT = 1,
    BOTTOM_RIGHT = 2,
    BOTTOM_LEFT = 3,
    TOP = 4,
    RIGHT = 5,
    BOTTOM = 6,
    LEFT = 7,
    CENTER = 8,
};

struct GUIWindow {
    std::string name;
    std::vector<GUIComponent*> components;

    // Optional free-form ImGui drawing, called each frame inside the open
    // window after the components (render thread). Use it for anything the
    // component types don't cover: slots, icons, custom layouts.
    std::function<void()> customContent;

    // Anchor plus a pixel offset from it. The offset always points *inward*
    // from whichever edge(s) the anchor names
    ScreenAnchor anchor = ScreenAnchor::TOP_LEFT;
    float offsetX = 0.0f;
    float offsetY = 0.0f;

    uint16_t sizeX = 0;
    uint16_t sizeY = 0;
};

} // namespace Volcano

#endif
