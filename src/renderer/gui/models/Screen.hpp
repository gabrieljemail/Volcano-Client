#pragma once
#ifndef GUI_SCREEN_H
#define GUI_SCREEN_H

#include <functional>
#include <string>
#include <vector>
#include "GUIComponent.hpp"

namespace Volcano::GUI {

// A fullscreen, Minecraft-like menu screen (e.g. the connect screen, a
// future pause/options menu). Only one Screen is ever active at a time —
// GUIController draws it in place of every other window and blocks game
// input until it's closed.
struct Screen {
    std::string title;
    std::vector<GUIComponent*> components;
    bool closable = true; // whether Esc is allowed to dismiss this screen

    // Escape hatch for content the generic TEXT/BUTTON/INPUT component stack
    // can't express — a slot grid with item icons (the inventory screen),
    // say. When set, GUIController::RenderScreen calls this instead of
    // laying out `components`, right after the title — the darkened
    // fullscreen "##screen" ImGui window (and its draw list, via
    // ImGui::GetWindowDrawList()) is already open by the time this runs, so
    // a raw-ImDrawList screen can draw into it the same way GUIController's
    // own HUD methods (DrawSlot, DrawStatBar, ...) already do.
    std::function<void()> customContent;
};

} // namespace Volcano::GUI

#endif
