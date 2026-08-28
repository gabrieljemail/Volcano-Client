#pragma once
#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <atomic>


namespace Volcano {

struct Resolution {
    uint16_t x;
    uint16_t y;
};

struct InputState {
    // The camera yaw, usually controlled by the mouse or a gamepad collective input.
    float cameraX = 0.0;
    // The camera pitch, usually controlled by the mouse or a gamepad collective input.
    float cameraY = 0.0;

    // Keys currently pressed.
    std::vector<uint32_t> keysDown();
    // Keys pressed this frame.
    std::vector<uint32_t> keysPressed();
};

struct GlobalState {
    std::map<std::string, std::variant<uint8_t, uint16_t, uint32_t, const char*>*> settings;
    InputState input;

    // Current state:
    bool shouldClose{false};
    Resolution resolution;
    uint16_t targetFPS{60};
    uint16_t currentFPS{0};

    // Load settings.
    void LoadSettings()
    {}
};

}

#endif