#pragma once
#ifndef GUI_SCREEN_H
#define GUI_SCREEN_H

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
};

} // namespace Volcano::GUI

#endif
