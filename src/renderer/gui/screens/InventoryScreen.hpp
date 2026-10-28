#pragma once
#ifndef VOLCANO_GUI_SCREENS_INVENTORY_SCREEN_H
#define VOLCANO_GUI_SCREENS_INVENTORY_SCREEN_H

#include "../models/Screen.hpp"
#include "GlobalState.hpp"

namespace Volcano::GUI {

// The player's inventory, and whatever container the server has open (crafting
// table, furnace, chests...), drawn with Pincel: the vanilla panel art with an
// Image in each slot. Pincel reports which slot got which mouse button's click,
// and this decides what that means (picking up, quick moving, dropping...).
// The screen's title is the open container's name; the caller sets it.
Screen* CreateInventoryScreen(GlobalState* state);

} // namespace Volcano::GUI

#endif
