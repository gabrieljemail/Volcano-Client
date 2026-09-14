#pragma once
#ifndef VOLCANO_ITEM_STACK_H
#define VOLCANO_ITEM_STACK_H

#include <cstdint>

namespace Volcano {

// One inventory slot's contents — mirrors the protocol's Slot data
// structure (a present flag, item id, and count) closely enough for this
// client's needs; the data-components/NBT part of Slot (enchantments, custom
// names, ...) isn't modeled yet since nothing parses inventory packets off
// the wire yet either (see InventoryManager). A default-constructed
// ItemStack is an empty slot — itemId 0 doubles as "no item" (item id 0 is
// "minecraft:air" in minecraft-data's items.json, so this needs no separate
// bool to stay in sync with count the way a real Slot's "Present" flag
// would), matching the itemId/visualId-0-means-nothing convention
// BlockRegistry already uses for block state ids.
struct ItemStack {
    int32_t itemId = 0;
    uint8_t count = 0;

    bool IsEmpty() const { return itemId == 0 || count == 0; }
};

} // namespace Volcano

#endif
