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

// Mouse buttons share the same keysDown/keysPressedThisFrame/
// keysReleasedThisFrame bitsets as keyboard keys (see InputState) rather
// than getting their own — offsetting them past GLFW_KEY_LAST, instead of
// using GLFW's own 0-7 button codes directly, means they stay unambiguous
// even if a future GLFW header widens the key range. RegisterAction's
// `triggers` accepts these the same as any GLFW_KEY_* constant.
constexpr size_t MOUSE_BUTTON_OFFSET = GLFW_KEY_LAST + 1;
constexpr uint16_t MOUSE_BUTTON_LEFT = static_cast<uint16_t>(MOUSE_BUTTON_OFFSET + GLFW_MOUSE_BUTTON_LEFT);
constexpr uint16_t MOUSE_BUTTON_RIGHT = static_cast<uint16_t>(MOUSE_BUTTON_OFFSET + GLFW_MOUSE_BUTTON_RIGHT);

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
// Owned exclusively by the main/render thread: KeyCallback (fired by
// glfwPollEvents(), called from PollInputs() at the top of every frame — see
// the tick-loop plan's thread-merge step) and every IsKeyDown/WasActivated/
// GetAxis reader all run on that one thread now, so there's nothing left to
// race. This used to be a snapshot ProcessFrame() copied from a
// mutex-guarded live/pending split written by GLFW callbacks on a genuinely
// different (main, pre-merge) thread — see git history for that shape if
// the render loop is ever split back onto its own thread.
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
        glfwSetMouseButtonCallback(window, MouseButtonCallback);

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

    // Resolves this frame's axis values from whatever KeyCallback/
    // CursorPosCallback wrote directly into `state`/mouseDeltaX/Y during
    // this frame's glfwPollEvents() (called at the top of PollInputs(),
    // just before this). Kept as its own named call — matching the render
    // loop's WaitForTargetFrame -> UpdateDeltaTime -> PollInputs -> ...
    // shape — even though it's just UpdateAxes() now that there's no
    // cross-thread pending/live split left to drain.
    void ProcessFrame()
    {
        UpdateAxes();
    }

    // The render loop's explicit "this frame is over" marker: clears the
    // press/release edge bits (so an edge is only ever visible for the one
    // frame it happened in) and the accumulated mouse delta (so the next
    // frame's CursorPosCallback calls start accumulating from zero) — see
    // CursorPosCallback's own comment for why mouseDeltaX/Y accumulate
    // rather than overwrite.
    void EndFrame()
    {
        state.keysPressedThisFrame.reset();
        state.keysReleasedThisFrame.reset();
        mouseDeltaX = 0.0;
        mouseDeltaY = 0.0;
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
    InputState state; // Main/render-thread-owned; see InputState's own comment.
    std::unordered_map<std::string, InputAction> actions;
    std::unordered_map<std::string, InputAxis> axes;

    // Accumulated by CursorPosCallback across every mouse-move event this
    // frame's glfwPollEvents() dispatches, read by UpdateAxes(), then reset
    // to zero by EndFrame() — see EndFrame's own comment.
    double mouseDeltaX = 0.0, mouseDeltaY = 0.0;
    double lastMouseX = 0.0, lastMouseY = 0.0;
    bool firstMouseEvent = true;

    // Fired by glfwPollEvents() (called from PollInputs(), on this same
    // thread) — writes straight into `state` with no locking needed now
    // that nothing else touches it from another thread. See InputState's
    // own comment for why that's safe.
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

    // Fired by glfwPollEvents(), same as KeyCallback — writes into the same
    // bitsets, offset by MOUSE_BUTTON_OFFSET (see its own comment).
    static void MouseButtonCallback(GLFWwindow* win, int button, int action, int mods)
    {
        (void)mods;
        auto* self = static_cast<InputHandler*>(glfwGetWindowUserPointer(win));
        if (!self || button < 0) return;
        size_t key = MOUSE_BUTTON_OFFSET + static_cast<size_t>(button);
        if (key >= MAX_KEYS) return;

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
    }

    static void CursorPosCallback(GLFWwindow* win, double xpos, double ypos)
    {
        auto* self = static_cast<InputHandler*>(glfwGetWindowUserPointer(win));
        #ifdef _DEBUG
            if (!self) throw std::runtime_error("[ERROR] Failed to get self (InputHandler) instance.");
        #endif
        if (!self) return;

        if (self->firstMouseEvent)
        {
            self->lastMouseX = xpos;
            self->lastMouseY = ypos;
            self->firstMouseEvent = false;
            return;
        }

        // Accumulate rather than overwrite: glfwPollEvents() can dispatch
        // several cursor-move events in one call (a fast mouse polling
        // faster than the frame rate), and every bit of motion this frame
        // should count toward this frame's look delta, not just the last
        // event before EndFrame() resets it to zero.
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
