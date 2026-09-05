#pragma once
#ifndef GUI_BUTTON_H
#define GUI_BUTTON_H

#include <string>
#include <vector>
#include <functional>
#include "./GUIComponent.hpp"

namespace Volcano::GUI {

enum ButtonPriority {
    NORMAL,
    PRIMARY,
    SECONDARY
};

enum ButtonHoverState {
    NONE,
    HOVERED,
    ACTIVE
};

class Button : public GUI::GUIComponent {
public:
    explicit Button(const std::string& label, ButtonPriority buttonPriority = ButtonPriority::NORMAL)
        : GUIComponent(GUIComponentType::BUTTON, label), priority(buttonPriority), hoverState(ButtonHoverState::NONE)
    {}

    virtual void AddClickHandler(std::function<void()>* callback) override { clickHandlers.push_back(callback); }
    virtual void RemoveClickHandler(std::function<void()>* callback) override { std::erase(clickHandlers, callback); }
    void SetHoverState(ButtonHoverState newHoverState) { hoverState = newHoverState; }
    virtual void Click() override
    {
        // hoverState = ButtonHoverState::ACTIVE;
        for (std::function<void()>* handler : clickHandlers)
        {
            (*handler)();
        }
    };

    ButtonPriority GetPriority() { return priority; }
    std::vector<std::function<void()>*> GetClickHandlers() { return clickHandlers; }

private:
    ButtonPriority priority;
    ButtonHoverState hoverState;
    std::vector<std::function<void()>*> clickHandlers;
};

} // namespace Volcano

#endif
