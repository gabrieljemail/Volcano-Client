#pragma once
#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include <cstdint>
#include <string>
#include <vector>
#include "GUIComponent.hpp"

namespace Volcano::GUI {

struct GUIWindow {
    std::string name;
    std::vector<GUIComponent*> components;
    uint16_t positionX = 0;
    uint16_t positionY = 0;
    uint16_t sizeX = 0;
    uint16_t sizeY = 0;
};

} // namespace Volcano

#endif
