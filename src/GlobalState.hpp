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
#include "InputHandler.hpp"
#include "Player.hpp"

namespace Volcano {

struct Resolution {
    uint16_t x;
    uint16_t y;
};

struct GlobalState {
    std::map<std::string, std::variant<uint8_t, uint16_t, uint32_t, const char*>*> settings;
    InputHandler* input;
    Player* player;
    GLFWwindow* window;

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
