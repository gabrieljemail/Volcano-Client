#pragma once
#ifndef VOLCANO_SLOT_GROUP_H
#define VOLCANO_SLOT_GROUP_H

#include <algorithm>
#include <string>
#include <vector>
#include "ItemStack.hpp"

namespace Volcano {

// A fixed-size, named group of item slots
class SlotGroup {
public:
    SlotGroup(std::string label, size_t size) : label(std::move(label)), slots(size) {}

    size_t Size() const { return slots.size(); }
    const std::string& Label() const { return label; }

    // Returns a shared empty ItemStack for an out-of-range index rather than
    // throwing
    ItemStack& Get(size_t index) {
        static ItemStack empty{};
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
