#pragma once
#ifndef VOLCANO_GUI_SCREENS_SETTINGS_SCREEN_H
#define VOLCANO_GUI_SCREENS_SETTINGS_SCREEN_H

#include "../models/Screen.hpp"

namespace Volcano::GUI {

// Placeholder for the real per-category settings screens (see this phase's
// step 4) — exists now so both the main menu and the pause menu can share
// one Settings entry point. Its Back button returns to whichever screen
// opened it, which is why it's opened through OpenSettingsScreen rather than
// GUIController::OpenScreen directly.
Screen* CreateSettingsScreen();
void OpenSettingsScreen(Screen* settingsScreen, Screen* returnTo);

} // namespace Volcano::GUI

#endif
