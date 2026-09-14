#pragma once
#ifndef VOLCANO_SLOT_GROUP_H
#define VOLCANO_SLOT_GROUP_H

#include <algorithm>
#include <string>
#include <vector>
#include "ItemStack.hpp"

namespace Volcano {

// A fixed-size, named group of item slots — the hotbar, the main inventory
// grid, the four armor slots, the single offhand slot — anywhere a Window
// Items/Set Container Slot packet's flat slot index can land once inventory
// packet handling exists (see InventoryManager::SetSlot). Fixed-size rather
// than exposing a plain std::vector<ItemStack> so an out-of-range index is a
// no-op/empty-read instead of a silent vector resize or out-of-bounds UB.
class SlotGroup {
public:
    SlotGroup(std::string label, size_t size) : label(std::move(label)), slots(size) {}

    size_t Size() const { return slots.size(); }
    const std::string& Label() const { return label; }

    // Returns a shared empty ItemStack for an out-of-range index rather than
    // throwing — every caller site (rendering a fixed N-slot HUD strip,
    // routing a packet's slot index) already has to handle "this slot has
    // nothing in it" as a normal case, so an invalid index reading the same
    // way keeps callers simple instead of needing a separate bounds check.
    const ItemStack& Get(size_t index) const {
        static const ItemStack empty{};
        return index < slots.size() ? slots[index] : empty;
    }

    void Set(size_t index, const ItemStack& item) {
        if (index < slots.size()) slots[index] = item;
    }

    void Clear() {
        std::fill(slots.begin(), slots.end(), ItemStack{});
    }

private:
    std::string label;
    std::vector<ItemStack> slots;
};

} // namespace Volcano

#endif
