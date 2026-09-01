#pragma once
#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <bitset>
#include <GLFW/glfw3.h>

namespace Volcano {

constexpr size_t MAX_KEYS = 512; // covers GLFW_KEY_LAST (~348) with headroom for mouse buttons

enum class InputActionTriggerType {
    PRESS,
    HOLD,
    RELEASE
};

struct InputAction {
    InputActionTriggerType triggerType;
    std::vector<uint16_t> triggers;
};

enum class AxisSourceType {
    KeyPair,   // two discrete keys, e.g. A/D
    MouseX,
    MouseY,
    // GamepadStickX, GamepadStickY, etc. later
};

struct InputAxis {
    float min;
    float max;
    float value = 0.0f;
    float sensitivity = 0.1f;    // only used for mouse axes
    uint16_t positiveTrigger;    // only used for KeyPair
    uint16_t negativeTrigger;
    AxisSourceType axisSource;
    bool snapTap;
};

// Cache-line aligned so polling this every frame doesn't thrash the cache
// with unrelated data, per the spec's InputState design.
struct alignas(64) InputState {
    std::bitset<MAX_KEYS> keysDown;
    std::bitset<MAX_KEYS> keysPressedThisFrame;
    std::bitset<MAX_KEYS> keysReleasedThisFrame;
};

class InputHandler {
public:
    explicit InputHandler(GLFWwindow* win) : window(win)
    {
        glfwSetWindowUserPointer(window, this);
        glfwSetKeyCallback(window, KeyCallback);
        glfwSetCursorPosCallback(window, CursorPosCallback);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }

    void ProcessFrame()
    {
        state.keysPressedThisFrame.reset();
        state.keysReleasedThisFrame.reset();
        mouseDeltaX = 0.0;
        mouseDeltaY = 0.0;

        glfwPollEvents(); // callbacks fire synchronously within this call

        UpdateAxes();
    }

    void RegisterAction(const std::string& name, InputActionTriggerType triggerType,
                         std::vector<uint16_t> triggers)
    {
        actions[name] = InputAction{ triggerType, std::move(triggers) };
    }

    void RegisterAxis(const std::string& name, InputAxis axis)
    {
        axes[name] = axis;
    }

    bool IsActive(std::string_view name) const
    {
        auto it = actions.find(std::string(name));
        if (it == actions.end()) return false;
        const InputAction& action = it->second;

        for (uint16_t key : action.triggers)
        {
            switch (action.triggerType)
            {
                case InputActionTriggerType::HOLD:
                    if (state.keysDown[key]) return true;
                    break;
                case InputActionTriggerType::PRESS:
                    if (state.keysPressedThisFrame[key]) return true;
                    break;
                case InputActionTriggerType::RELEASE:
                    if (state.keysReleasedThisFrame[key]) return true;
                    break;
            }
        }
        return false;
    }

    bool WasActivated(std::string_view name) const
    {
        auto it = actions.find(std::string(name));
        if (it == actions.end()) return false;

        for (uint16_t key : it->second.triggers)
            if (state.keysPressedThisFrame[key]) return true;
        return false;
    }

    float GetAxis(std::string_view name) const
    {
        auto it = axes.find(std::string(name));
        if (it == axes.end()) return 0.0f;
        return it->second.value;
    }

private:
    GLFWwindow* window;
    InputState state;
    std::unordered_map<std::string, InputAction> actions;
    std::unordered_map<std::string, InputAxis> axes;
    double lastMouseX = 0.0, lastMouseY = 0.0;
    double mouseDeltaX = 0.0, mouseDeltaY = 0.0;
    bool firstMouseEvent = true;

    static void KeyCallback(GLFWwindow* win, int key, int scancode, int action, int mods)
    {
        auto* self = static_cast<InputHandler*>(glfwGetWindowUserPointer(win));
        if (!self || key < 0 || static_cast<size_t>(key) >= MAX_KEYS) return;

        if (action == GLFW_PRESS)
        {
            self->state.keysDown.set(key);
            self->state.keysPressedThisFrame.set(key);
        }
        else if (action == GLFW_RELEASE)
        {
            self->state.keysDown.reset(key);
            self->state.keysReleasedThisFrame.set(key);
        }
        // GLFW_REPEAT: keysDown already true, nothing to change.
    }

    static void CursorPosCallback(GLFWwindow* win, double xpos, double ypos)
    {
        auto* self = static_cast<InputHandler*>(glfwGetWindowUserPointer(win));
        if (!self) return;

        if (self->firstMouseEvent)
        {
            self->lastMouseX = xpos;
            self->lastMouseY = ypos;
            self->firstMouseEvent = false;
            return;
        }

        self->mouseDeltaX += xpos - self->lastMouseX;
        self->mouseDeltaY += ypos - self->lastMouseY;
        self->lastMouseX = xpos;
        self->lastMouseY = ypos;
    }

    void UpdateAxes()
    {
        for (auto& [name, axis] : axes)
        {
            switch (axis.axisSource)
            {
                case AxisSourceType::KeyPair:
                {
                    float raw = 0.0f;
                    if (state.keysDown[axis.positiveTrigger]) raw += 1.0f;
                    if (state.keysDown[axis.negativeTrigger]) raw -= 1.0f;
                    axis.value = raw;
                    break;
                }
                case AxisSourceType::MouseX:
                    axis.value = static_cast<float>(mouseDeltaX) * axis.sensitivity;
                    break;
                case AxisSourceType::MouseY:
                    axis.value = static_cast<float>(mouseDeltaY) * axis.sensitivity;
                    break;
            }
        }
    }
};

} // namespace Volcano

#endif
