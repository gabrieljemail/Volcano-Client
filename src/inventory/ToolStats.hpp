#pragma once
#ifndef VOLCANO_TOOL_STATS_H
#define VOLCANO_TOOL_STATS_H

#include <string>
#include "../renderer/terrain/models/BlockRegistry.hpp"

namespace Volcano {

// What a held item does to mining speed: its tool type and tier speed.
struct ToolStats {
    BlockRegistry::ToolClass toolClass = BlockRegistry::ToolClass::None;
    float speed = 1.0f; // Mining speed multiplier on blocks of its own class.
};

// Reads the type and tier off a tool's name ("diamond_pickaxe"). Vanilla's
// tier speeds; anything that isn't a pickaxe/axe/shovel/hoe is a bare hand.
inline ToolStats ToolStatsForItem(const std::string& itemName)
{
    ToolStats stats;
    auto endsWith = [&](const char* suffix)
    {
        std::string s(suffix);
        return itemName.size() >= s.size() && itemName.compare(itemName.size() - s.size(), s.size(), s) == 0;
    };

    if (endsWith("_pickaxe")) stats.toolClass = BlockRegistry::ToolClass::Pickaxe;
    else if (endsWith("_axe")) stats.toolClass = BlockRegistry::ToolClass::Axe;
    else if (endsWith("_shovel")) stats.toolClass = BlockRegistry::ToolClass::Shovel;
    else if (endsWith("_hoe")) stats.toolClass = BlockRegistry::ToolClass::Hoe;
    else return stats;

    auto startsWith = [&](const char* prefix) { return itemName.rfind(prefix, 0) == 0; };
    if (startsWith("wooden_")) stats.speed = 2.0f;
    else if (startsWith("stone_")) stats.speed = 4.0f;
    else if (startsWith("copper_")) stats.speed = 5.0f;
    else if (startsWith("iron_")) stats.speed = 6.0f;
    else if (startsWith("diamond_")) stats.speed = 8.0f;
    else if (startsWith("netherite_")) stats.speed = 9.0f;
    else if (startsWith("golden_")) stats.speed = 12.0f;
    return stats;
}

// Melee numbers shown in tooltips: total damage on a fully charged hit and
// attack speed (hits per second), from vanilla's per-tier tables.
struct AttackStats {
    bool valid = false;
    float damage = 1.0f;
    float speed = 4.0f;
};

inline AttackStats AttackStatsForItem(const std::string& itemName)
{
    AttackStats stats;
    auto endsWith = [&](const char* suffix)
    {
        std::string s(suffix);
        return itemName.size() >= s.size() && itemName.compare(itemName.size() - s.size(), s.size(), s) == 0;
    };
    auto startsWith = [&](const char* prefix) { return itemName.rfind(prefix, 0) == 0; };

    // Index: wooden, stone, copper, iron, golden, diamond, netherite.
    int tier = startsWith("wooden_") ? 0 : startsWith("stone_") ? 1 : startsWith("copper_") ? 2
        : startsWith("iron_") ? 3 : startsWith("golden_") ? 4 : startsWith("diamond_") ? 5
        : startsWith("netherite_") ? 6 : -1;

    if (itemName == "trident") { stats = { true, 9.0f, 1.1f }; return stats; }
    if (itemName == "mace") { stats = { true, 6.0f, 0.6f }; return stats; }
    if (tier < 0) return stats;

    static const float sword[] = { 4, 5, 5, 6, 4, 7, 8 };
    static const float axe[] = { 7, 9, 9, 9, 7, 9, 10 };
    static const float axeSpeed[] = { 0.8f, 0.8f, 0.9f, 0.9f, 1.0f, 1.0f, 1.0f };
    static const float pickaxe[] = { 2, 3, 3, 4, 2, 5, 6 };
    static const float shovel[] = { 2.5f, 3.5f, 3.5f, 4.5f, 2.5f, 5.5f, 6.5f };
    static const float hoeSpeed[] = { 1.0f, 2.0f, 2.0f, 3.0f, 1.0f, 4.0f, 4.0f };

    if (endsWith("_sword")) stats = { true, sword[tier], 1.6f };
    else if (endsWith("_axe")) stats = { true, axe[tier], axeSpeed[tier] };
    else if (endsWith("_pickaxe")) stats = { true, pickaxe[tier], 1.2f };
    else if (endsWith("_shovel")) stats = { true, shovel[tier], 1.0f };
    else if (endsWith("_hoe")) stats = { true, 1.0f, hoeSpeed[tier] };
    return stats;
}

} // namespace Volcano

#endif
