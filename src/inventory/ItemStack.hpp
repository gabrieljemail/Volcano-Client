#pragma once
#ifndef VOLCANO_ITEM_STACK_H
#define VOLCANO_ITEM_STACK_H

#include <cstdint>

namespace Volcano {

// One inventory slot's contents
struct ItemStack {
    int32_t itemId = 0;
    uint8_t count = 0;
    // From the damage / max_damage components; -1 when the stack carried none.
    int32_t damage = -1;
    int32_t maxDamage = -1;

    bool IsEmpty() const { return itemId == 0 || count == 0; }
};

} // namespace Volcano

#endif
