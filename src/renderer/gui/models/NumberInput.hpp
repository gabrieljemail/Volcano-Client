#pragma once
#ifndef VOLCANO_GUI_NUMBER_INPUT_H
#define VOLCANO_GUI_NUMBER_INPUT_H

#include <cstdint>
#include <functional>
#include <string>
#include "GUIComponent.hpp"

namespace Volcano::GUI {

// Integer field bound through a getter/setter pair. Commits when editing
// finishes (Enter or focus loss), not on every keystroke.
class NumberInput : public GUIComponent
{
public:
    NumberInput(const std::string& label, int32_t min, int32_t max,
                std::function<int32_t()> getter, std::function<void(int32_t)> setter)
        : GUIComponent(GUIComponentType::NUMBER, label), min(min), max(max),
          getter(std::move(getter)), setter(std::move(setter))
    {}

    int32_t GetMin() const { return min; }
    int32_t GetMax() const { return max; }
    int32_t GetValue() const { return getter(); }
    void SetValue(int32_t value) { if (value != getter()) setter(value); }

    // Value being typed, kept separate so partial input isn't committed.
    int32_t editValue = 0;
    bool editing = false;

private:
    int32_t min, max;
    std::function<int32_t()> getter;
    std::function<void(int32_t)> setter;
};

} // namespace Volcano::GUI

#endif
