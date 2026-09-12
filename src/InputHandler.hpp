#pragma once
#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <bitset>
#include <mutex>
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
//
// Owned exclusively by the render thread: only ProcessFrame() writes to it
// (snapshotting the live, main-thread-written state below), so every
// IsKeyDown/WasActivated/GetAxis read during a frame is race-free and sees
// one consistent picture for that whole frame.
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

#ifdef _WIN32
        // On Windows, GLFW_FOCUSED can already read true here even though
        // the window hasn't actually been shown/composited by the OS yet —
        // disabling the cursor this early clips it to a window the desktop
        // hasn't painted, which looks like a visible cursor trapped in a
        // "ghost" window until the user clicks it. Leave the cursor alone;
        // VulkanInit's first-frame-presented hook (and its focus callback
        // after that) own locking it once the window is genuinely up.
#else
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
#endif
    }

    // Takes everything the GLFW callbacks have accumulated since the last
    // call and makes it this frame's input picture. Runs at the start of
    // the render loop's PollInputs().
    //
    // The pending/drain split matters: GLFW callbacks fire on the MAIN
    // thread (whose glfwPollEvents() loop is the only thing that dispatches
    // them), at arbitrary moments relative to the render thread's frame
    // boundaries. Edge bits used to be written straight into `state` and
    // cleared by EndFrame() at the end of each render frame — so a press
    // that landed after a frame's readers had already run (very likely,
    // since DrawFrame() then blocks for most of the frame in
    // vkWaitForFences/vkQueuePresentKHR under FIFO/VSync) was wiped before
    // any reader ever saw it. That's why edge-triggered actions — Jump,
    // F11, Escape, chat's "T" — effectively never fired, while HOLD-style
    // input (WASD, reading keysDown) always worked. Latching into
    // `pending*` and draining here means a press can't be dropped no
    // matter when it arrives; it's simply observed by the next frame.
    void ProcessFrame()
    {
        {
            std::lock_guard<std::mutex> lock(inputMutex);
            state.keysDown = liveKeysDown;
            state.keysPressedThisFrame = pendingPressed;
            state.keysReleasedThisFrame = pendingReleased;
            pendingPressed.reset();
            pendingReleased.reset();

            mouseDeltaX = pendingMouseDeltaX;
            mouseDeltaY = pendingMouseDeltaY;
            pendingMouseDeltaX = 0.0;
            pendingMouseDeltaY = 0.0;
        }

        UpdateAxes(); // Reads the mouseDeltaX/Y just drained above.
    }

    // Kept as the render loop's explicit "this frame is over" marker, but
    // the edge bits it used to clear are now bounded by ProcessFrame()'s
    // drain instead (see above) — clearing here as well just means an
    // edge is never visible past the frame that drained it, even if
    // ProcessFrame() isn't reached next iteration.
    void EndFrame()
    {
        state.keysPressedThisFrame.reset();
        state.keysReleasedThisFrame.reset();
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

    // Raw key queries, for one-off bindings (e.g. Esc closing a Screen)
    // that don't warrant registering a named action.
    bool IsKeyDown(int key) const
    {
        if (key < 0 || static_cast<size_t>(key) >= MAX_KEYS) return false;
        return state.keysDown[key];
    }

    bool IsKeyPressed(int key) const
    {
        if (key < 0 || static_cast<size_t>(key) >= MAX_KEYS) return false;
        return state.keysPressedThisFrame[key];
    }

private:
    GLFWwindow* window;
    InputState state; // Render-thread-owned snapshot; see InputState's own comment.
    std::unordered_map<std::string, InputAction> actions;
    std::unordered_map<std::string, InputAxis> axes;
    double mouseDeltaX = 0.0, mouseDeltaY = 0.0; // Drained from pendingMouseDelta* by ProcessFrame.

    // Written by the GLFW callbacks on the main thread, drained by
    // ProcessFrame() on the render thread — all under inputMutex. See
    // ProcessFrame()'s comment for why edge state has to be latched here
    // rather than written straight into `state`.
    std::mutex inputMutex;
    std::bitset<MAX_KEYS> liveKeysDown;
    std::bitset<MAX_KEYS> pendingPressed;
    std::bitset<MAX_KEYS> pendingReleased;
    double lastMouseX = 0.0, lastMouseY = 0.0;
    double pendingMouseDeltaX = 0.0, pendingMouseDeltaY = 0.0;
    bool firstMouseEvent = true;

    static void KeyCallback(GLFWwindow* win, int key, int scancode, int action, int mods)
    {
        auto* self = static_cast<InputHandler*>(glfwGetWindowUserPointer(win));
        if (!self || key < 0 || static_cast<size_t>(key) >= MAX_KEYS) return;

        std::lock_guard<std::mutex> lock(self->inputMutex);
        if (action == GLFW_PRESS)
        {
            self->liveKeysDown.set(key);
            self->pendingPressed.set(key);
        }
        else if (action == GLFW_RELEASE)
        {
            self->liveKeysDown.reset(key);
            self->pendingReleased.set(key);
        }
        // GLFW_REPEAT: liveKeysDown already true, nothing to change.
    }

    static void CursorPosCallback(GLFWwindow* win, double xpos, double ypos)
    {
        auto* self = static_cast<InputHandler*>(glfwGetWindowUserPointer(win));
        #ifdef _DEBUG
            if (!self) throw std::runtime_error("[ERROR] Failed to get self (InputHandler) instance.");
        #endif
        if (!self) return;

        std::lock_guard<std::mutex> lock(self->inputMutex);
        if (self->firstMouseEvent)
        {
            self->lastMouseX = xpos;
            self->lastMouseY = ypos;
            self->firstMouseEvent = false;
            return;
        }

        self->pendingMouseDeltaX += xpos - self->lastMouseX;
        self->pendingMouseDeltaY += ypos - self->lastMouseY;
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
                    axis.value = static_cast<float>(mouseDeltaX) * -axis.sensitivity;
                    break;
                case AxisSourceType::MouseY:
                    axis.value = static_cast<float>(mouseDeltaY) * -axis.sensitivity;
                    break;
            }
        }
    }
};

} // namespace Volcano

#endif
