#pragma once
#ifndef VOLCANO_INVENTORY_MANAGER_H
#define VOLCANO_INVENTORY_MANAGER_H

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "SlotGroup.hpp"

namespace Volcano {

struct InventoryManager {
    static constexpr size_t MAIN_INVENTORY_SIZE = 27;
    static constexpr size_t HOTBAR_SIZE = 9;
    static constexpr size_t ARMOR_SIZE = 4; // helmet, chestplate, leggings, boots - see SetSlot's protocol index mapping.
    // Protocol MenuType ids. 26.1 inserted crafter_3x3 (7), shifting these up
    // by one from the 1.20 values (11/13). Unverified against the registry,
    // check the "Open window" log line if the layouts look wrong.
    static constexpr int32_t CRAFTING_MENU_TYPE = 12;
    static constexpr int32_t FURNACE_MENU_TYPE = 14;

    std::mutex mutex;

    // A copy of the player's inventory, safe to hold and draw from without the
    // lock. Slot order matches the groups below: hotbar 0-8, main 0-26 (the
    // top row first), armor head-to-feet.
    struct Snapshot {
        std::array<ItemStack, HOTBAR_SIZE> hotbar;
        std::array<ItemStack, MAIN_INVENTORY_SIZE> main;
        std::array<ItemStack, ARMOR_SIZE> armor;
        ItemStack offhand;
        ItemStack carried;
        uint8_t selectedHotbarSlot = 0;
    };

    // Takes the lock itself; never call it while holding `mutex`.
    Snapshot TakeSnapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        Snapshot snapshot;
        for (size_t i = 0; i < HOTBAR_SIZE; i++) snapshot.hotbar[i] = hotbar.Get(i);
        for (size_t i = 0; i < MAIN_INVENTORY_SIZE; i++) snapshot.main[i] = mainInventory.Get(i);
        for (size_t i = 0; i < ARMOR_SIZE; i++) snapshot.armor[i] = armor.Get(i);
        snapshot.offhand = offhand.Get(0);
        snapshot.carried = carriedItem;
        snapshot.selectedHotbarSlot = selectedHotbarSlot;
        return snapshot;
    }

    SlotGroup mainInventory{"Main Inventory", MAIN_INVENTORY_SIZE};
    SlotGroup hotbar{"Hotbar", HOTBAR_SIZE};
    SlotGroup armor{"Armor", ARMOR_SIZE};
    SlotGroup offhand{"Offhand", 1};
    // The player inventory's own 2x2 crafting grid: protocol index 0 is the
    // result, 1-4 the grid (row-major).
    SlotGroup crafting{"Crafting", 5};

    // The item being moved with the mouse.
    ItemStack carriedItem{};

    int32_t playerStateId = 0;
    int32_t activeWindowStateId = 0;

    int32_t activeWindowId = 0;
    int32_t activeWindowType = 0;
    std::string activeWindowTitle;
    std::vector<ItemStack> containerSlots;
    bool openWindowPending = false;
    bool closeWindowPending = false;

    uint8_t selectedHotbarSlot = 0;

    // Helper to get mutable reference by protocol slot index
    ItemStack* GetSlot(int32_t protocolIndex)
    {
        if (protocolIndex >= 0 && protocolIndex <= 4) {
            return &crafting.Get(static_cast<size_t>(protocolIndex));
        } else if (protocolIndex >= 5 && protocolIndex <= 8) {
            return &armor.Get(static_cast<size_t>(protocolIndex - 5));
        } else if (protocolIndex >= 9 && protocolIndex <= 35) {
            return &mainInventory.Get(static_cast<size_t>(protocolIndex - 9));
        } else if (protocolIndex >= 36 && protocolIndex <= 44) {
            return &hotbar.Get(static_cast<size_t>(protocolIndex - 36));
        } else if (protocolIndex == 45) {
            return &offhand.Get(0);
        }
        return nullptr;
    }

    ItemStack* GetWindowSlot(int32_t protocolIndex)
    {
        if (activeWindowId == 0) return GetSlot(protocolIndex);
        if (protocolIndex < 0) return nullptr;
        if (static_cast<size_t>(protocolIndex) < containerSlots.size())
            return &containerSlots[static_cast<size_t>(protocolIndex)];

        int32_t playerSlot = protocolIndex - static_cast<int32_t>(containerSlots.size());
        if (playerSlot >= 0 && playerSlot < static_cast<int32_t>(MAIN_INVENTORY_SIZE))
            return &mainInventory.Get(static_cast<size_t>(playerSlot));
        playerSlot -= static_cast<int32_t>(MAIN_INVENTORY_SIZE);
        if (playerSlot >= 0 && playerSlot < static_cast<int32_t>(HOTBAR_SIZE))
            return &hotbar.Get(static_cast<size_t>(playerSlot));
        return nullptr;
    }

    void SetWindowContents(const std::vector<ItemStack>& items, const ItemStack& cursorItem)
    {
        constexpr size_t PLAYER_SLOTS = MAIN_INVENTORY_SIZE + HOTBAR_SIZE;
        size_t playerSlotCount = std::min(items.size(), PLAYER_SLOTS);
        size_t containerSlotCount = items.size() - playerSlotCount;
        containerSlots.assign(items.begin(), items.begin() + containerSlotCount);

        for (size_t i = 0; i < MAIN_INVENTORY_SIZE; i++)
        {
            size_t itemIndex = containerSlotCount + i;
            if (itemIndex < items.size()) mainInventory.Set(i, items[itemIndex]);
        }
        for (size_t i = 0; i < HOTBAR_SIZE; i++)
        {
            size_t itemIndex = containerSlotCount + MAIN_INVENTORY_SIZE + i;
            if (itemIndex < items.size()) hotbar.Set(i, items[itemIndex]);
        }
        carriedItem = cursorItem;
    }

    void SetWindowSlot(int32_t protocolIndex, const ItemStack& item)
    {
        if (protocolIndex < 0) return;
        if (static_cast<size_t>(protocolIndex) < containerSlots.size())
        {
            containerSlots[static_cast<size_t>(protocolIndex)] = item;
            return;
        }

        int32_t playerSlot = protocolIndex - static_cast<int32_t>(containerSlots.size());
        if (playerSlot >= 0 && playerSlot < static_cast<int32_t>(MAIN_INVENTORY_SIZE))
        {
            mainInventory.Set(static_cast<size_t>(playerSlot), item);
            return;
        }
        playerSlot -= static_cast<int32_t>(MAIN_INVENTORY_SIZE);
        if (playerSlot >= 0 && playerSlot < static_cast<int32_t>(HOTBAR_SIZE))
            hotbar.Set(static_cast<size_t>(playerSlot), item);
    }

    void OpenServerWindow(int32_t windowId, int32_t windowType, std::string title)
    {
        activeWindowId = windowId;
        activeWindowType = windowType;
        activeWindowTitle = std::move(title);
        containerSlots.clear();
        carriedItem = ItemStack{};
        activeWindowStateId = 0;
        openWindowPending = true;
        closeWindowPending = false;
    }

    void CloseServerWindow()
    {
        activeWindowId = 0;
        activeWindowType = 0;
        activeWindowTitle.clear();
        containerSlots.clear();
        carriedItem = ItemStack{};
        closeWindowPending = true;
        openWindowPending = false;
    }

    void HandleSlotClick(int32_t protocolIndex, uint8_t button /* 0 = Left, 1 = Right */)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (protocolIndex == -999)
        {
            if (button == 0) carriedItem = ItemStack{};
            else if (button == 1 && !carriedItem.IsEmpty())
            {
                carriedItem.count--;
                if (carriedItem.count == 0) carriedItem = ItemStack{};
            }
            return;
        }

        ItemStack* slot = GetWindowSlot(protocolIndex);
        if (slot == nullptr) return;

        // On left click.
        if (button == 0)
        {
            if (carriedItem.IsEmpty())
            {
                carriedItem = *slot;
                *slot = ItemStack{};
            }
            else if (slot->IsEmpty())
            {
                *slot = carriedItem;
                carriedItem = ItemStack{};
            }
            else if (slot->itemId == carriedItem.itemId)
            {
                uint16_t total = slot->count + carriedItem.count;
                if (total <= 64)
                {
                    slot->count = static_cast<uint8_t>(total);
                    carriedItem = ItemStack{};
                }
                else
                {
                    slot->count = 64;
                    carriedItem.count = static_cast<uint8_t>(total - 64);
                }
            }
            else {
                std::swap(*slot, carriedItem);
            }
        }
        else if (button == 1)
        {
            if (carriedItem.IsEmpty() && !slot->IsEmpty())
            {
                uint8_t half = (slot->count + 1) / 2;
                carriedItem = {slot->itemId, half};
                slot->count -= half;
                if (slot->count == 0) *slot = ItemStack{};
            }
            else if (!carriedItem.IsEmpty())
            {
                if (slot->IsEmpty())
                {
                    *slot = {carriedItem.itemId, 1};
                    carriedItem.count--;
                    if (carriedItem.count == 0) carriedItem = ItemStack{};
                }
                else if (slot->itemId == carriedItem.itemId && slot->count < 64)
                {
                    slot->count++;
                    carriedItem.count--;
                    if (carriedItem.count == 0) carriedItem = ItemStack{};
                }
            }
        }
    }

    void SetSlot(int32_t protocolIndex, const ItemStack& item)
    {
        if (protocolIndex >= 0 && protocolIndex <= 4) {
            crafting.Set(static_cast<size_t>(protocolIndex), item);
        } else if (protocolIndex >= 5 && protocolIndex <= 8) {
            armor.Set(static_cast<size_t>(protocolIndex - 5), item);
        } else if (protocolIndex >= 9 && protocolIndex <= 35) {
            mainInventory.Set(static_cast<size_t>(protocolIndex - 9), item);
        } else if (protocolIndex >= 36 && protocolIndex <= 44) {
            hotbar.Set(static_cast<size_t>(protocolIndex - 36), item);
        } else if (protocolIndex == 45) {
            offhand.Set(0, item);
        }
    }
};

} // namespace Volcano

#endif
