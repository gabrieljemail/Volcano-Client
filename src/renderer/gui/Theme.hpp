#pragma once
#ifndef VOLCANO_GUI_THEME_H
#define VOLCANO_GUI_THEME_H

#include <Pincel/Style.hpp>

namespace Volcano::GUI::Theme {

// The Volcano colors every Pincel-drawn screen and HUD piece shares.
constexpr Pincel::Color PANEL_DARK{ 10, 10, 10 };
constexpr Pincel::Color PANEL_MID{ 26, 26, 26 };
constexpr Pincel::Color PANEL_LIGHT{ 41, 41, 41 };
constexpr Pincel::Color ORANGE{ 255, 181, 16 };
constexpr Pincel::Color ORANGE_DIM{ 244, 90, 11 };
constexpr Pincel::Color LABEL_TEXT{ 242, 242, 242 };
constexpr Pincel::Color TEXT_DIM{ 153, 153, 153 };
constexpr Pincel::Color BUTTON_FILL{ 16, 16, 16 };
constexpr Pincel::Color BUTTON_BORDER{ 32, 32, 32 };
constexpr Pincel::Color BUTTON_TEXT{ 235, 248, 255 };
constexpr Pincel::Color SCREEN_DIM{ 0, 0, 0, 140.0f / 255.0f };
constexpr Pincel::Color SHADOW{ 0, 0, 0 };

// Colors of a command typed in the chat box, from the chat design. A namespaced id like minecraft:diamond_ore is
// NAMESPACE_NAME then CLASS_NAME. INVALID_WORD and DIM aren't in the design's palette.
namespace Syntax {
constexpr Pincel::Color COMMAND{ 0x39, 0xBA, 0xE6 };        // The command itself: /execute, /data.
constexpr Pincel::Color NAMESPACE_NAME{ 0xFF, 0xB4, 0x54 };
constexpr Pincel::Color CLASS_NAME{ 0x59, 0xC2, 0xFF };     // Types, targets, ids.
constexpr Pincel::Color STRING{ 0xAA, 0xD9, 0x4C };
constexpr Pincel::Color NUMBER{ 0xE6, 0xB4, 0x50 };
constexpr Pincel::Color KEY{ 0xF2, 0x97, 0x18 };            // NBT and selector keys, paths.
constexpr Pincel::Color OPERATOR{ 0xF0, 0x71, 0x78 };       // = { } [ ] , :
constexpr Pincel::Color CONST_PARAMETER{ 0xFF, 0x77, 0x33 }; // Subcommands, enum values, true, false.
constexpr Pincel::Color PLAIN{ 0xBF, 0xBD, 0xB6 };
constexpr Pincel::Color INVALID_WORD{ 255, 85, 85 };
constexpr Pincel::Color DIM{ 0x6C, 0x73, 0x80 };            // A word still being typed.
} // namespace Syntax

// Font size of the dense screens and the HUD (inventory, chat, hotbar, loading), in art pixels: one pixel of
// the pixel font is one pixel of the vanilla art, which is (GUI scale + 1) screen pixels, as in vanilla.
// So 8 is the font's own size, drawn at the same scale as the hotbar.
constexpr float SMALL_FONT_SIZE = 8.0f;

} // namespace Volcano::GUI::Theme

#endif
