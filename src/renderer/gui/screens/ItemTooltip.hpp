#pragma once
#ifndef VOLCANO_GUI_ITEM_TOOLTIP_H
#define VOLCANO_GUI_ITEM_TOOLTIP_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <Pincel/Style.hpp>
#include "inventory/ItemRegistry.hpp"
#include "inventory/ItemStack.hpp"
#include "inventory/ToolStats.hpp"

namespace Volcano::GUI {

// Shared item-tooltip text for the inventory-style screens (InventoryScreen,
// CreativeInventoryScreen): name, food values, durability, melee numbers.

using Pincel::Color;

// Vanilla's legacy formatting colors (the section sign plus 0-9, a-f).
inline bool LegacyColor(char code, Color& color)
{
    static const Color colors[16] = {
        { 0, 0, 0 }, { 0, 0, 170 }, { 0, 170, 0 }, { 0, 170, 170 }, { 170, 0, 0 }, { 170, 0, 170 }, { 255, 170, 0 }, { 170, 170, 170 },
        { 85, 85, 85 }, { 85, 85, 255 }, { 85, 255, 85 }, { 85, 255, 255 }, { 255, 85, 85 }, { 255, 85, 255 }, { 255, 255, 85 }, { 255, 255, 255 } };
    const char* digits = "0123456789abcdef";
    const char* at = std::strchr(digits, std::tolower(static_cast<unsigned char>(code)));
    if (code == '\0' || at == nullptr) return false;
    color = colors[at - digits];
    return true;
}

struct TooltipLine {
    std::string text; // May carry section-sign color codes (UTF-8 U+00A7).
    Color color{ 255, 255, 255 };
};

inline Color RarityColor(const std::string& name)
{
    if (name == "enchanted_golden_apple" || name == "dragon_egg" || name == "elytra" || name == "command_block")
        return Color{ 255, 85, 255 };
    if (name == "golden_apple" || name == "beacon" || name == "conduit" || name == "heart_of_the_sea"
        || name == "end_crystal" || name == "totem_of_undying" || name == "nether_star")
        return Color{ 85, 255, 255 };
    return Color{ 255, 255, 255 };
}

// Name, then food values, durability, and melee numbers where they apply.
inline std::vector<TooltipLine> TooltipLines(const ItemStack& item)
{
    std::vector<TooltipLine> lines;
    const ItemRegistry::ItemTypeInfo* info = ItemRegistry::Lookup(item.itemId);
    if (info == nullptr) return lines;

    lines.push_back({ info->displayName.empty() ? info->name : info->displayName, RarityColor(info->name) });

    char buffer[96];
    if (info->edible)
    {
        std::snprintf(buffer, sizeof(buffer), "Hunger: +%d", info->foodPoints);
        lines.push_back({ buffer, Color{ 255, 170, 0 } });
        std::snprintf(buffer, sizeof(buffer), "Saturation: +%.1f", static_cast<double>(info->saturation));
        lines.push_back({ buffer, Color{ 255, 220, 90 } });
    }

    int32_t maxDurability = item.maxDamage > 0 ? item.maxDamage : info->maxDurability;
    if (maxDurability > 0)
    {
        int32_t remaining = std::clamp(maxDurability - std::max(item.damage, 0), 0, maxDurability);
        float fraction = static_cast<float>(remaining) / static_cast<float>(maxDurability);
        std::snprintf(buffer, sizeof(buffer), "Durability: %d / %d", remaining, maxDurability);
        lines.push_back({ buffer, fraction > 0.5f ? Color{ 85, 255, 85 } : fraction > 0.2f ? Color{ 255, 255, 85 } : Color{ 255, 85, 85 } });
    }

    AttackStats attack = AttackStatsForItem(info->name);
    if (attack.valid)
    {
        lines.push_back({ " ", Color{ 255, 255, 255 } });
        lines.push_back({ "When in Main Hand:", Color{ 170, 170, 170 } });
        std::snprintf(buffer, sizeof(buffer), "%.4g Attack Damage (fully charged)", static_cast<double>(attack.damage));
        lines.push_back({ buffer, Color{ 85, 85, 255 } });
        std::snprintf(buffer, sizeof(buffer), "%.4g Attack Speed", static_cast<double>(attack.speed));
        lines.push_back({ buffer, Color{ 85, 85, 255 } });
        std::snprintf(buffer, sizeof(buffer), "Fully charged after %.2f s", static_cast<double>(1.0f / attack.speed));
        lines.push_back({ buffer, Color{ 170, 170, 170 } });
    }
    return lines;
}

// A line split on its color codes into spans.
inline std::vector<Pincel::TextSpan> SpansOf(const TooltipLine& line)
{
    std::vector<Pincel::TextSpan> spans;
    Color color = line.color;
    std::string segment;
    auto flush = [&]()
    {
        if (segment.empty()) return;
        Pincel::TextSpan span;
        span.text = segment;
        span.color = color;
        spans.push_back(std::move(span));
        segment.clear();
    };
    for (size_t i = 0; i < line.text.size(); i++)
    {
        bool sectionSign = static_cast<unsigned char>(line.text[i]) == 0xC2 && i + 2 < line.text.size()
            && static_cast<unsigned char>(line.text[i + 1]) == 0xA7;
        if (sectionSign)
        {
            char code = line.text[i + 2];
            flush();
            if (code == 'r' || code == 'R') color = line.color;
            else LegacyColor(code, color);
            i += 2;
            continue;
        }
        segment.push_back(line.text[i]);
    }
    flush();
    return spans;
}

} // namespace Volcano::GUI

#endif
