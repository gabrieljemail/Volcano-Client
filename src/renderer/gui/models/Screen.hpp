#pragma once
#ifndef GUI_SCREEN_H
#define GUI_SCREEN_H

#include <functional>
#include <string>
#include <vector>
#include "GUIComponent.hpp"

namespace Volcano::GUI {

class ControlRenderer;

// A fullscreen, Minecraft-like menu screen (e.g. the connect screen, a
// future pause/options menu). Only one Screen is ever active at a time -
// GUIController draws it in place of every other window and blocks game
// input until it's closed.
struct Screen {
    std::string title;
    std::vector<GUIComponent*> components;
    bool closable = true; // whether Esc is allowed to dismiss this screen
    // Columns for labeled controls (sliders, options, number fields). Plain
    // buttons and text always span the full width, centered.
    int columns = 1;
    // Draw resources/logo.png (128x128) in place of the title text.
    bool showLogo = false;
    // False for a screen that draws its own title (or has none).
    bool showTitle = true;
    // Text size; 0 is the standard one. In UI units, or in art pixels (see Theme::SMALL_FONT_SIZE) if fontInArtPixels.
    float fontSize = 0.0f;
    bool fontInArtPixels = false;

    // Escape hatch for content the generic TEXT/BUTTON/INPUT component stack
    // can't express
    std::function<void()> customContent;

    // For content that changes while the screen is up (a list of servers) but
    // still wants the standard look. `build` adds elements after the components
    // whenever the screen is built, `update` runs every frame after drawing to
    // react to them, and ControlRenderer::Rebuild() runs `build` again. Both
    // run on the render thread; a screen with either is drawn by Pincel.
    std::function<void(ControlRenderer&)> build;
    std::function<void(ControlRenderer&)> update;
};

} // namespace Volcano::GUI

#endif
