#pragma once
#ifndef VOLCANO_GUI_SCREENS_SETTINGS_SCREEN_H
#define VOLCANO_GUI_SCREENS_SETTINGS_SCREEN_H

#include "../models/Screen.hpp"

namespace Volcano {
struct GlobalState;
}

namespace Volcano::GUI {

// Settings hub plus its category sub-screens (Video, Window, Audio, Controls).
// Every control writes straight to settings.json through Config::Set, and
// the systems that own each setting pick the change up live. Back returns
// to whichever screen opened it, hence OpenSettingsScreen.
Screen* CreateSettingsScreen(GlobalState* state);
void OpenSettingsScreen(Screen* settingsScreen, Screen* returnTo);

} // namespace Volcano::GUI

#endif
