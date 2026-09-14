#pragma once
#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include <cstdint>
#include <string>
#include <vector>
#include "GUIComponent.hpp"

namespace Volcano::GUI {

// Which screen edge/corner a GUIWindow is pinned to. The window's on-screen
// position is resolved fresh every frame from the *current* display size
// (see GUIController::ResolveAnchor) plus a pixel offset — never baked in
// at creation time — so a window stays correctly placed across window
// resizes and window-mode changes (e.g. F11 into/out of fullscreen), which
// a one-time absolute position computed from the launch resolution can't.
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

    // Anchor plus a pixel offset from it. The offset always points *inward*
    // from whichever edge(s) the anchor names — e.g. offsetX on TOP_LEFT
    // pushes right, on TOP_RIGHT pushes left — so the same positive margin
    // works regardless of which corner/edge a window is pinned to.
    ScreenAnchor anchor = ScreenAnchor::TOP_LEFT;
    float offsetX = 0.0f;
    float offsetY = 0.0f;

    uint16_t sizeX = 0;
    uint16_t sizeY = 0;
};

} // namespace Volcano

#endif
