#pragma once
#include "models/GUIWindow.hpp"
#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <atomic>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include "renderer/models/Mesh.hpp"
#include "renderer/gui/models/GUIWindow.hpp"
#include "renderer/terrain/models/World.hpp"
#include "InputHandler.hpp"
#include "Player.hpp"
#include "PlayerAttributes.hpp"

namespace Volcano {

// Defined in TickLoop.hpp, which includes this header (not the other way
// around) since it needs the full GlobalState — a forward declaration is
// enough here for a pointer member.
class TickLoop;

struct Resolution {
    uint16_t x;
    uint16_t y;
};

struct GlobalState {
    std::map<std::string, std::variant<uint8_t, uint16_t, uint32_t, const char*>*> settings;
    InputHandler* input;
    Player* player;
    GLFWwindow* window;
    World* world;
    PlayerAttributes* attributes;
    TickLoop* tickLoop;

    // Current state:
    bool shouldClose{false};
    Resolution resolution;
    uint16_t targetFPS{60};
    uint16_t currentFPS{0};
    std::vector<Mesh> renderList = {};

    glm::vec3 cameraPosition{0.0f, 0.0f, 5.0f};

    // Load settings.
    void LoadSettings()
    {}
};

}

#endif
