#pragma once
#ifndef VOLCANO_ITEM_BEHAVIOR_H
#define VOLCANO_ITEM_BEHAVIOR_H

#include <cstdint>
#include <string>
#include "ItemRegistry.hpp"

namespace Volcano {

// What holding the secondary button does with an item, held until released
// or finished. Anything else (blocks, ender pearls, ...) is an instant use.
enum class UseAction : uint8_t {
    None,
    Eat,
    Drink,
    Block,    // Shield.
    Bow,
    Crossbow,
    Spear,    // Trident and spears.
    Spyglass,
    Horn,
    Brush,
};

// What the client needs to know about a held item to predict a use.
struct ItemBehavior {
    UseAction action = UseAction::None;
    int32_t useTicks = 0;     // Ticks until a use finishes by itself; 0 = until released.
    bool alwaysEdible = false; // Usable at full hunger.
    int32_t armorSlot = -1;   // Index into InventoryManager::armor (0 = helmet .. 3 = boots), or -1.
};

namespace ItemBehaviorDetail
{
inline bool EndsWith(const std::string& s, const char* suffix)
{
    std::string t(suffix);
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}
}

// Derived from the item's name; items.json carries no behavior components.
inline ItemBehavior ItemBehaviorFor(const ItemRegistry::ItemTypeInfo* item)
{
    using ItemBehaviorDetail::EndsWith;
    ItemBehavior behavior;
    if (item == nullptr) return behavior;
    const std::string& name = item->name;

    if (EndsWith(name, "_helmet") || name == "carved_pumpkin" || EndsWith(name, "_head") || EndsWith(name, "_skull"))
    {
        behavior.armorSlot = 0;
    }
    else if (EndsWith(name, "_chestplate") || name == "elytra") behavior.armorSlot = 1;
    else if (EndsWith(name, "_leggings")) behavior.armorSlot = 2;
    else if (EndsWith(name, "_boots")) behavior.armorSlot = 3;

    if (name == "shield") behavior.action = UseAction::Block;
    else if (name == "bow") behavior.action = UseAction::Bow;
    else if (name == "crossbow") behavior.action = UseAction::Crossbow;
    else if (name == "trident" || EndsWith(name, "_spear")) behavior.action = UseAction::Spear;
    else if (name == "spyglass") behavior.action = UseAction::Spyglass;
    else if (name == "goat_horn") { behavior.action = UseAction::Horn; behavior.useTicks = 140; }
    else if (name == "brush") { behavior.action = UseAction::Brush; behavior.useTicks = 200; }
    else if (name == "potion" || name == "milk_bucket" || name == "honey_bottle" || name == "ominous_bottle")
    {
        behavior.action = UseAction::Drink;
        behavior.useTicks = name == "honey_bottle" ? 40 : 32;
        behavior.alwaysEdible = true;
    }
    else if (item->edible)
    {
        behavior.action = UseAction::Eat;
        behavior.useTicks = name == "dried_kelp" ? 16 : 32;
        behavior.alwaysEdible = name == "golden_apple" || name == "enchanted_golden_apple" || name == "chorus_fruit";
    }
    return behavior;
}

// True if the mainhand item would act on a clicked block, so the offhand
// gets no turn. Weapons and an empty hand don't.
inline bool UsesOnBlocks(const ItemRegistry::ItemTypeInfo* item)
{
    using ItemBehaviorDetail::EndsWith;
    if (item == nullptr) return false;
    const std::string& name = item->name;
    return !(EndsWith(name, "_sword") || EndsWith(name, "_pickaxe") || name == "mace");
}

} // namespace Volcano

#endif
