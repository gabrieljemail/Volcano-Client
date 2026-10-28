#pragma once
#ifndef VOLCANO_GUI_SCREENS_LOADING_SCREEN_H
#define VOLCANO_GUI_SCREENS_LOADING_SCREEN_H

#include <functional>
#include "../models/Screen.hpp"
#include "GlobalState.hpp"

namespace Volcano::GUI {

// The single screen shown from the moment a connection attempt starts until
// the world is ready (GlobalState::worldReady). What it says is driven by
// GlobalState::connectionPhase/connectionDetail rather than being separate
// screens per step: "Starting server..." (with the live server log),
// "Connecting to host:port...", "Configuring..." (with the Configuration
// packet currently being processed), then "Loading terrain...".
//
// Doesn't open or close itself
Screen* CreateLoadingScreen(GlobalState* state, std::function<void()> onCancel);

} // namespace Volcano::GUI

#endif
