#pragma once
#ifndef VOLCANO_GUI_SCREENS_CREATIVE_INVENTORY_SCREEN_H
#define VOLCANO_GUI_SCREENS_CREATIVE_INVENTORY_SCREEN_H

#include "../models/Screen.hpp"
#include "GlobalState.hpp"

namespace Volcano::GUI {

// The creative-mode inventory, drawn with Pincel: a searchable item picker on
// the left (category rail + scrolling grid) and the player's own inventory on
// the right. There is no server window behind it - picker clicks only move the
// client-side cursor stack, and every player-inventory edit is reported with
// PlayC2S::SetCreativeSlot (slot -1 drops into the world), the same model
// vanilla's CreativeInventoryListener uses.
Screen* CreateCreativeScreen(GlobalState* state);

} // namespace Volcano::GUI

#endif
