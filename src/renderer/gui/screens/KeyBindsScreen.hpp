#pragma once
#ifndef VOLCANO_GUI_SCREENS_KEY_BINDS_SCREEN_H
#define VOLCANO_GUI_SCREENS_KEY_BINDS_SCREEN_H

#include <functional>
#include "../models/Screen.hpp"

namespace Volcano {
struct GlobalState;
}

namespace Volcano::GUI {

// Lists every KeyBindings entry by category. Click a key, then press the new
// key or mouse button (Esc cancels, Backspace/Delete unbinds); a key shared
// with another control is shown in red. Changes apply at once and are saved
// to settings.json (Controls.Keys.*). `onDone` runs for the Done button.
Screen* CreateKeyBindsScreen(GlobalState* state, std::function<void()> onDone);

} // namespace Volcano::GUI

#endif
