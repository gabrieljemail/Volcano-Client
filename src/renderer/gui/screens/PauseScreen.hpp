#pragma once
#ifndef VOLCANO_GUI_SCREENS_PAUSE_SCREEN_H
#define VOLCANO_GUI_SCREENS_PAUSE_SCREEN_H

#include <functional>
#include "../models/Screen.hpp"

namespace Volcano::GUI {

// The in-game pause menu (Esc while playing — see GUIController::Update):
// Resume, Servers (leave this server and pick another), Settings, and
// Disconnect (back to the main menu). Resume is handled here; the other
// three need the connection and the other screens, which only the caller
// (VolcanoClient.cpp) has, so they're callbacks. Hand the result to
// GUIController::SetPauseScreen.
Screen* CreatePauseScreen(std::function<void()> onServers,
                          std::function<void()> onSettings,
                          std::function<void()> onDisconnect);

} // namespace Volcano::GUI

#endif
