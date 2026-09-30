#pragma once
#ifndef VOLCANO_GUI_OPTION_BUTTON_H
#define VOLCANO_GUI_OPTION_BUTTON_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include "GUIComponent.hpp"

namespace Volcano::GUI {

// Vanilla-style "Label: Value" button that cycles through a list of options
// on click. The option list can be filled lazily (e.g. monitor video modes,
// which need GLFW running) via SetOptionProvider.
class OptionButton : public GUIComponent
{
public:
    OptionButton(const std::string& label, std::vector<std::string> options,
                 std::function<size_t()> getter, std::function<void(size_t)> setter)
        : GUIComponent(GUIComponentType::OPTION, label), options(std::move(options)),
          getter(std::move(getter)), setter(std::move(setter))
    {}

    void SetOptionProvider(std::function<std::vector<std::string>()> provider) { optionProvider = std::move(provider); }

    std::string GetDisplayText()
    {
        EnsureOptions();
        size_t index = getter();
        return GetLabel() + ": " + (index < options.size() ? options[index] : "?");
    }

    void Click() override
    {
        EnsureOptions();
        if (options.empty()) return;
        setter((getter() + 1) % options.size());
    }

private:
    void EnsureOptions()
    {
        if (options.empty() && optionProvider) options = optionProvider();
    }

    std::vector<std::string> options;
    std::function<size_t()> getter;
    std::function<void(size_t)> setter;
    std::function<std::vector<std::string>()> optionProvider;
};

} // namespace Volcano::GUI

#endif
