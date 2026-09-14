#pragma once
#ifndef VOLCANO_INVENTORY_MANAGER_H
#define VOLCANO_INVENTORY_MANAGER_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include "SlotGroup.hpp"

namespace Volcano {

// Player inventory state — the slot groups a Window Items/Set Container
// Slot packet's contents ultimately land in, once NetworkClient actually
// parses those (not implemented yet; this only defines where the data goes
// so that work can just call SetSlot()/populate these groups directly
// rather than also having to design the storage). Read by the future
// inventory/hotbar/armor GUI.
//
// Follows GlobalState's usual convention for state written by the network
// thread and read by the render thread: a plain data member with a mutex
// right next to it, locked manually by both sides — see e.g. GlobalState's
// entities/entitiesMutex or playerList/playerListMutex for the same shape.
// Held directly as a GlobalState member (not behind a pointer) the same way
// GlobalState::networkInbox is, since there's exactly one per session and no
// benefit to separate allocation/lifetime management.
struct InventoryManager {
    static constexpr size_t MAIN_INVENTORY_SIZE = 27;
    static constexpr size_t HOTBAR_SIZE = 9;
    static constexpr size_t ARMOR_SIZE = 4; // helmet, chestplate, leggings, boots — see SetSlot's protocol index mapping.

    std::mutex mutex;

    SlotGroup mainInventory{"Main Inventory", MAIN_INVENTORY_SIZE};
    SlotGroup hotbar{"Hotbar", HOTBAR_SIZE};
    SlotGroup armor{"Armor", ARMOR_SIZE};
    SlotGroup offhand{"Offhand", 1};

    // Which hotbar slot (0-8) is currently held/selected — set by the
    // future Set Held Item packet, read by the future held-item/hotbar-
    // highlight GUI. Not the same thing as any SlotGroup index; it's a
    // separate piece of state vanilla tracks alongside the slots themselves.
    uint8_t selectedHotbarSlot = 0;

    // Routes one flat protocol slot index — the player inventory window's
    // (id 0) own indexing: 0 = crafting output, 1-4 = crafting grid, 5-8 =
    // armor (helmet/chestplate/leggings/boots), 9-35 = main inventory,
    // 36-44 = hotbar, 45 = offhand — to the right SlotGroup, so future
    // packet handling can hand a Window Items/Set Container Slot packet's
    // indices straight through here instead of re-deriving this mapping
    // itself. Crafting (0-4) isn't modeled yet (there's no crafting UI to
    // read it back from), so those indices are silently dropped rather than
    // stored somewhere nothing will ever read from. Caller must hold `mutex`
    // — this only routes to the right Set() call, it doesn't lock anything
    // itself, matching every other cross-thread member in this struct.
    void SetSlot(int32_t protocolIndex, const ItemStack& item) {
        if (protocolIndex >= 5 && protocolIndex <= 8) {
            armor.Set(static_cast<size_t>(protocolIndex - 5), item);
        } else if (protocolIndex >= 9 && protocolIndex <= 35) {
            mainInventory.Set(static_cast<size_t>(protocolIndex - 9), item);
        } else if (protocolIndex >= 36 && protocolIndex <= 44) {
            hotbar.Set(static_cast<size_t>(protocolIndex - 36), item);
        } else if (protocolIndex == 45) {
            offhand.Set(0, item);
        }
        // 0 (crafting output) and 1-4 (crafting grid): not modeled yet, dropped.
    }
};

} // namespace Volcano

#endif
