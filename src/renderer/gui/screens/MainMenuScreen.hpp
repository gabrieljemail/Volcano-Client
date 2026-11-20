#pragma once
#ifndef VOLCANO_GUI_SCREENS_MAIN_MENU_SCREEN_H
#define VOLCANO_GUI_SCREENS_MAIN_MENU_SCREEN_H

#include "../models/Screen.hpp"
#include "ApplicationDeps.hpp"
#ifdef VOLCANO_MODULES
import Volcano;
#else
#include "Application.hpp"
#endif

namespace Volcano::GUI {

// Builds the main menu
Screen* CreateMainMenuScreen(Application* state, Screen* singleplayerScreen, Screen* multiplayerScreen,
    Screen* settingsScreen);

} // namespace Volcano::GUI

#endif
