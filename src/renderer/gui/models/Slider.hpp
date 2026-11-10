#pragma once
#ifndef VOLCANO_GUI_SLIDER_H
#define VOLCANO_GUI_SLIDER_H

#include <cstdint>
#include <functional>
#include <string>
#include "GUIComponent.hpp"

namespace Volcano::GUI {

// Integer slider bound through a getter/setter pair (e.g. a Config key).
// Commits once on release, not per drag step
class Slider : public GUIComponent
{
public:
    Slider(const std::string& label, int32_t min, int32_t max,
           std::function<int32_t()> getter, std::function<void(int32_t)> setter, const std::string& suffix = "")
        : GUIComponent(GUIComponentType::SLIDER, label), min(min), max(max),
          getter(std::move(getter)), setter(std::move(setter)), suffix(suffix)
    {}

    int32_t GetMin() const { return min; }
    int32_t GetMax() const { return max; }
    int32_t GetValue() const { return getter(); }
    void SetValue(int32_t value) { if (value != getter()) setter(value); }
    const std::string& GetSuffix() const { return suffix; }

    // Value being dragged, committed on release.
    int32_t editValue = 0;
    bool editing = false;

private:
    int32_t min, max;
    std::function<int32_t()> getter;
    std::function<void(int32_t)> setter;
    std::string suffix;
};

} // namespace Volcano::GUI

#endif
