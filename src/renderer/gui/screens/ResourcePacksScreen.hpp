#pragma once
#ifndef VOLCANO_GUI_SCREENS_RESOURCE_PACKS_SCREEN_H
#define VOLCANO_GUI_SCREENS_RESOURCE_PACKS_SCREEN_H

#include <functional>
#include "../models/Screen.hpp"

namespace Volcano {
struct GlobalState;
}

namespace Volcano::GUI {

// Manages Graphics.ResourcePacks: packs found in resourcepacks/ on the left,
// the active ones on the right in priority order (top wins), and the base
// pack pinned at the bottom since nothing renders without it. Changes are
// written to settings.json at once and the packs reload when Done is
// pressed. `onDone` runs after that.
Screen* CreateResourcePacksScreen(GlobalState* state, std::function<void()> onDone);

} // namespace Volcano::GUI

#endif
