#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <GLFW/glfw3.h>
#include "SettingsScreen.hpp"
#include "../GUIController.hpp"
#include "../models/Button.hpp"
#include "../models/GUIComponent.hpp"
#include "../models/NumberInput.hpp"
#include "../models/OptionButton.hpp"
#include "../models/Slider.hpp"
#include "../../../GlobalState.hpp"

namespace Volcano::GUI {

namespace
{
Screen* returnScreen = nullptr;

Button* MakeNavButton(const std::string& label, std::function<void()> onClick)
{
    auto* button = new Button(label);
    button->AddClickHandler(new std::function<void()>(std::move(onClick)));
    return button;
}

Slider* MakeSlider(Config* config, const std::string& label, const std::string& key,
                   int32_t min, int32_t max, uint32_t fallback, const std::string& suffix = "",
                   std::function<void(int32_t)> onChange = nullptr)
{
    return new Slider(label, min, max,
        [config, key, fallback] { return static_cast<int32_t>(std::get<uint32_t>(config->Get(key, fallback))); },
        [config, key, onChange](int32_t value)
        {
            config->Set(key, static_cast<uint32_t>(value));
            if (onChange) onChange(value);
        },
        suffix);
}

OptionButton* MakeToggle(Config* config, const std::string& label, const std::string& key, bool fallback)
{
    return new OptionButton(label, { "Off", "On" },
        [config, key, fallback] { return std::get<bool>(config->Get(key, fallback)) ? size_t{1} : size_t{0}; },
        [config, key](size_t index) { config->Set(key, index == 1); });
}

NumberInput* MakeNumber(Config* config, const std::string& label, const std::string& key,
                        int32_t min, int32_t max, uint32_t fallback)
{
    return new NumberInput(label, min, max,
        [config, key, fallback] { return static_cast<int32_t>(std::get<uint32_t>(config->Get(key, fallback))); },
        [config, key](int32_t value) { config->Set(key, static_cast<uint32_t>(value)); });
}

// Fullscreen resolution picker: the primary monitor's own mode list, so it
// can't produce an unsupported mode. Writes all three FullscreenMode keys.
OptionButton* MakeFullscreenModeOption(Config* config)
{
    auto modes = std::make_shared<std::vector<GLFWvidmode>>();
    auto loadModes = [modes]
    {
        if (!modes->empty()) return;
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        int count = 0;
        const GLFWvidmode* list = monitor ? glfwGetVideoModes(monitor, &count) : nullptr;
        for (int i = count - 1; i >= 0; i--) modes->push_back(list[i]); // largest first
    };

    auto* option = new OptionButton("Fullscreen", {},
        [config, modes, loadModes]
        {
            loadModes();
            uint32_t width = std::get<uint32_t>(config->Get("Window.FullscreenMode.Width", uint32_t{0}));
            uint32_t height = std::get<uint32_t>(config->Get("Window.FullscreenMode.Height", uint32_t{0}));
            uint32_t refresh = std::get<uint32_t>(config->Get("Window.FullscreenMode.RefreshRate", uint32_t{0}));
            for (size_t i = 0; i < modes->size(); i++)
            {
                const GLFWvidmode& mode = (*modes)[i];
                if (static_cast<uint32_t>(mode.width) == width && static_cast<uint32_t>(mode.height) == height
                    && static_cast<uint32_t>(mode.refreshRate) == refresh)
                {
                    return i;
                }
            }
            return size_t{0};
        },
        [config, modes](size_t index)
        {
            if (index >= modes->size()) return;
            const GLFWvidmode& mode = (*modes)[index];
            config->Set("Window.FullscreenMode.Width", static_cast<uint32_t>(mode.width));
            config->Set("Window.FullscreenMode.Height", static_cast<uint32_t>(mode.height));
            config->Set("Window.FullscreenMode.RefreshRate", static_cast<uint32_t>(mode.refreshRate));
        });

    option->SetOptionProvider([modes, loadModes]
    {
        loadModes();
        std::vector<std::string> names;
        for (const GLFWvidmode& mode : *modes)
        {
            names.push_back(std::to_string(mode.width) + "x" + std::to_string(mode.height)
                + " @ " + std::to_string(mode.refreshRate) + " Hz");
        }
        return names;
    });
    return option;
}

Screen* CreateVideoScreen(GlobalState* state, Screen* settingsScreen)
{
    Config* config = state->config;
    Screen* screen = GUIController::CreateScreen("Video Settings", false);
    screen->components.push_back(MakeSlider(config, "Render Distance", "Graphics.RenderDistance", 2, 32, 8, " chunks (rejoin)"));
    screen->components.push_back(MakeSlider(config, "FOV", "Graphics.FOV", 30, 110, 100));
    screen->components.push_back(MakeSlider(config, "FOV Effects", "Graphics.FOVEffects", 0, 20, 5));
    screen->components.push_back(MakeSlider(config, "Brightness", "Graphics.Brightness", 0, 100, 50, "%"));
    screen->components.push_back(MakeToggle(config, "Lighting", "Graphics.Lighting", true));
    screen->components.push_back(MakeSlider(config, "Max FPS", "Graphics.TargetFPS", 30, 360, 60, "",
        [state](int32_t value) { state->targetFPS = static_cast<uint16_t>(value); }));
    screen->components.push_back(MakeToggle(config, "VSync", "Graphics.VSync", true));
    screen->components.push_back(MakeSlider(config, "Swapchain Buffers", "Graphics.BufferCount", 2, 3, 2));
    screen->components.push_back(MakeNavButton("Done", [settingsScreen] { GUIController::OpenScreen(settingsScreen); }));
    return screen;
}

Screen* CreateWindowScreen(GlobalState* state, Screen* settingsScreen)
{
    Config* config = state->config;
    Screen* screen = GUIController::CreateScreen("Window Settings", false);
    screen->components.push_back(new OptionButton("Mode", { "Windowed", "Borderless", "Fullscreen" },
        [config] { return static_cast<size_t>(std::get<uint32_t>(config->Get("Window.Mode", uint32_t{0}))); },
        [config](size_t index) { config->Set("Window.Mode", static_cast<uint32_t>(index)); }));
    screen->components.push_back(MakeFullscreenModeOption(config));
    screen->components.push_back(MakeNumber(config, "Windowed Width", "Window.Width", 320, 7680, 854));
    screen->components.push_back(MakeNumber(config, "Windowed Height", "Window.Height", 240, 4320, 480));
    screen->components.push_back(MakeNavButton("Done", [settingsScreen] { GUIController::OpenScreen(settingsScreen); }));
    return screen;
}

Screen* CreateControlsScreen(GlobalState* state, Screen* settingsScreen)
{
    Config* config = state->config;
    Screen* screen = GUIController::CreateScreen("Controls", false);
    screen->components.push_back(MakeSlider(config, "Mouse Sensitivity", "Controls.Mouse.Sensitivity", 10, 300, 100, "%"));
    screen->components.push_back(MakeToggle(config, "Raw Input (restart)", "Controls.Mouse.RawInput", true));
    screen->components.push_back(MakeToggle(config, "Toggle Sprint", "Controls.ToggleSprint", false));
    screen->components.push_back(MakeNavButton("Done", [settingsScreen] { GUIController::OpenScreen(settingsScreen); }));
    return screen;
}
}

Screen* CreateSettingsScreen(GlobalState* state)
{
    // Not closable: Esc would leave no screen open when reached from the main menu.
    Screen* screen = GUIController::CreateScreen("Settings", false);
    Screen* videoScreen = CreateVideoScreen(state, screen);
    Screen* windowScreen = CreateWindowScreen(state, screen);
    Screen* controlsScreen = CreateControlsScreen(state, screen);

    screen->components.push_back(MakeNavButton("Video...", [videoScreen] { GUIController::OpenScreen(videoScreen); }));
    screen->components.push_back(MakeNavButton("Window...", [windowScreen] { GUIController::OpenScreen(windowScreen); }));
    screen->components.push_back(MakeNavButton("Controls...", [controlsScreen] { GUIController::OpenScreen(controlsScreen); }));
    screen->components.push_back(MakeNavButton("Back", []
    {
        if (returnScreen != nullptr) GUIController::OpenScreen(returnScreen);
    }));
    return screen;
}

void OpenSettingsScreen(Screen* settingsScreen, Screen* returnTo)
{
    returnScreen = returnTo;
    GUIController::OpenScreen(settingsScreen);
}

} // namespace Volcano::GUI
