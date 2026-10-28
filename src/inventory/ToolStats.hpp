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

} // namespace Volcano

#endif
