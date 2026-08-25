#pragma once
#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

#include <map>
#include <string>
#include <variant>
#include <cstdint>

using namespace std;

namespace Volcano {

struct Resolution {
    uint16_t x;
    uint16_t y;
};

struct GlobalState {
    map<string, variant<uint8_t, uint16_t, uint32_t, const char*>*> settings;

    // Current state:
    Resolution resolution;
    uint16_t targetFPS;
    uint16_t currentFPS;
};

}

#endif