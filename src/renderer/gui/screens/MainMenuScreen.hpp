#pragma once
#ifndef VOLCANO_GUI_SCREENS_MAIN_MENU_SCREEN_H
#define VOLCANO_GUI_SCREENS_MAIN_MENU_SCREEN_H

#include "../models/Screen.hpp"
#include "GlobalState.hpp"

namespace Volcano::GUI {

// Builds the main menu - the first screen shown at startup (VolcanoClient.cpp
// opens it in place of the old direct-to-connect-screen flow) and the hub
// every other top-level menu screen returns to via its own Back button.
//
// Doesn't take ownership of singleplayerScreen/multiplayerScreen/
// settingsScreen - the caller (currently VolcanoClient.cpp's main(), where
// they're built inline) keeps building and owning those; this just wires
// this screen's buttons to open them. settingsScreen is opened through
// OpenSettingsScreen so its Back button returns here.
Screen* CreateMainMenuScreen(GlobalState* state, Screen* singleplayerScreen, Screen* multiplayerScreen,
    Screen* settingsScreen);

} // namespace Volcano::GUI

#endif
