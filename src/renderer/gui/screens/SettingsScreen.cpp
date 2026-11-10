#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <GLFW/glfw3.h>
#include "SettingsScreen.hpp"
#include "KeyBindsScreen.hpp"
#include "ResourcePacksScreen.hpp"
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

template<typename T>
T* WithTooltip(T* component, const std::string& text)
{
    component->SetTooltip(text);
    return component;
}

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

// Fullscreen resolution picker: "Desktop" (0x0@0, the monitor's current
// mode), then the primary monitor's own mode list, so it can't produce an
// unsupported mode. A saved mode not in the list shows as Desktop, since
// that's what fullscreen falls back to. Writes all three FullscreenMode keys.
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
                    return i + 1;
                }
            }
            return size_t{0};
        },
        [config, modes](size_t index)
        {
            uint32_t width = 0, height = 0, refresh = 0;
            if (index > 0 && index <= modes->size())
            {
                const GLFWvidmode& mode = (*modes)[index - 1];
                width = static_cast<uint32_t>(mode.width);
                height = static_cast<uint32_t>(mode.height);
                refresh = static_cast<uint32_t>(mode.refreshRate);
            }
            config->Set("Window.FullscreenMode.Width", width);
            config->Set("Window.FullscreenMode.Height", height);
            config->Set("Window.FullscreenMode.RefreshRate", refresh);
        });

    option->SetOptionProvider([modes, loadModes]
    {
        loadModes();
        std::vector<std::string> names{ "Desktop" };
        for (const GLFWvidmode& mode : *modes)
        {
            names.push_back(std::to_string(mode.width) + "x" + std::to_string(mode.height)
                + " @ " + std::to_string(mode.refreshRate) + " Hz");
        }
        return names;
    });
    return option;
}

// Video is a hub of categories, each its own screen; Done returns to the hub.
Screen* CreateVideoCategory(const std::string& title)
{
    Screen* screen = GUIController::CreateScreen(title, false);
    screen->columns = 2;
    return screen;
}

Screen* CreateVideoScreen(GlobalState* state, Screen* settingsScreen)
{
    Config* config = state->config;
    Screen* hub = GUIController::CreateScreen("Video Settings", false);

    Screen* display = CreateVideoCategory("Display");
    {
        Screen* screen = display;
    screen->components.push_back(WithTooltip(new OptionButton("GUI Scale", { "Auto", "1x", "2x", "3x", "4x" },
        [config] { return static_cast<size_t>(std::min<uint32_t>(std::get<uint32_t>(config->Get("Graphics.GUIScale", uint32_t{0})), 4)); },
        [config](size_t index) { config->Set("Graphics.GUIScale", static_cast<uint32_t>(index)); }),
        "Size of menus and the HUD. Auto picks the largest size that fits the window."));
    screen->components.push_back(WithTooltip(MakeSlider(config, "FOV", "Graphics.FOV", 30, 110, 100),
        "Field of view in degrees. Higher shows more around you; lower zooms in."));
    screen->components.push_back(WithTooltip(MakeSlider(config, "FOV Effects", "Graphics.FOVEffects", 0, 20, 5),
        "How many degrees the view widens while sprinting. 0 turns the effect off."));
    screen->components.push_back(WithTooltip(MakeToggle(config, "View Bobbing", "Graphics.ViewBobbing", true),
        "Sways the view and your hand with each step as you walk."));
    screen->components.push_back(WithTooltip(MakeToggle(config, "Body View (Experimental)", "Graphics.BodyView", false),
        "Shows your own animated body and arms in first person, as others see you in third person, instead of the floating hand."));
    screen->components.push_back(WithTooltip(MakeSlider(config, "Damage Tilt", "Graphics.DamageTilt", 0, 100, 100, "%"),
        "How far the view rolls when you take a hit. 0 turns it off."));
    screen->components.push_back(WithTooltip(MakeSlider(config, "Hand Scale", "Graphics.HandScale", 25, 200, 100, "%"),
        "Size of the held item and hand in first person."));
    screen->components.push_back(WithTooltip(MakeToggle(config, "Left Handed", "Graphics.LeftHanded", false),
        "Swaps your main hand and off hand. The held item and hand move to the left side."));
    screen->components.push_back(WithTooltip(MakeSlider(config, "Brightness", "Graphics.Brightness", 0, 100, 50, "%"),
        "Brightens dark areas like caves and night without washing out daylight."));
        screen->components.push_back(MakeNavButton("Done", [hub] { GUIController::OpenScreen(hub); }));
    }

    Screen* quality = CreateVideoCategory("Quality");
    {
        Screen* screen = quality;
    screen->components.push_back(WithTooltip(MakeSlider(config, "Render Distance", "Graphics.RenderDistance", 2, 32, 8, " chunks"),
        "How far away chunks load. Higher costs more memory and FPS. Applies when you next join a world."));
    screen->components.push_back(WithTooltip(new OptionButton("Clouds", { "Off", "Fast", "Fancy" },
        [config] { return static_cast<size_t>(std::min<uint32_t>(std::get<uint32_t>(config->Get("Graphics.Clouds", uint32_t{2})), 2)); },
        [config](size_t index) { config->Set("Graphics.Clouds", static_cast<uint32_t>(index)); }),
        "Fast draws flat clouds, Fancy draws them with thickness."));
    screen->components.push_back(WithTooltip(new OptionButton("Particles", { "All", "Decreased", "Minimal" },
        [config] { return static_cast<size_t>(std::min<uint32_t>(std::get<uint32_t>(config->Get("Graphics.Particles", uint32_t{0})), 2)); },
        [config](size_t index) { config->Set("Graphics.Particles", static_cast<uint32_t>(index)); }),
        "How many particles are drawn. The server is told too, and sends fewer. Decreased keeps about a third, Minimal about a tenth."));
    screen->components.push_back(WithTooltip(MakeToggle(config, "Smooth Lighting", "Graphics.SmoothLighting", true),
        "Blends light across block corners instead of lighting each face flat. Applies to chunks as they re-mesh."));
    screen->components.push_back(WithTooltip(MakeSlider(config, "Specular", "Graphics.Specular", 0, 100, 50, "%"),
        "Sun and moon glints on shiny blocks like glass, ice, metal blocks and obsidian. 0 turns it off."));
    screen->components.push_back(WithTooltip(MakeToggle(config, "Lighting", "Graphics.Lighting", true),
        "Off renders everything fully lit, ignoring light levels."));
        screen->components.push_back(MakeNavButton("Done", [hub] { GUIController::OpenScreen(hub); }));
    }

    Screen* shadows = CreateVideoCategory("Shadows");
    {
        Screen* screen = shadows;
    screen->components.push_back(WithTooltip(MakeToggle(config, "Shadows (Experimental)", "Graphics.Shadows.Enabled", false),
        "Casts the sun's shadows from terrain, plants, mobs and players. Experimental: costs GPU time."));
    screen->components.push_back(WithTooltip(new OptionButton("Shadow Resolution", { "512", "1024", "2048", "4096" },
        [config] { return static_cast<size_t>(std::min<uint32_t>(std::get<uint32_t>(config->Get("Graphics.Shadows.Resolution", uint32_t{1})), 3)); },
        [config](size_t index) { config->Set("Graphics.Shadows.Resolution", static_cast<uint32_t>(index)); }),
        "Shadow map size in pixels. Higher is sharper and costs more memory and time."));
    screen->components.push_back(WithTooltip(MakeSlider(config, "Shadow Distance", "Graphics.Shadows.Distance", 16, 512, 128, " blocks"),
        "How far from you shadows are drawn."));
    screen->components.push_back(WithTooltip(new OptionButton("Shadow Updates", { "Every Frame", "Client Tick", "Server Tick" },
        [config] { return static_cast<size_t>(std::min<uint32_t>(std::get<uint32_t>(config->Get("Graphics.Shadows.RecomputeOn", uint32_t{0})), 2)); },
        [config](size_t index) { config->Set("Graphics.Shadows.RecomputeOn", static_cast<uint32_t>(index)); }),
        "How often the shadow map is redrawn. Every Frame is smoothest. Client Tick (20 a second) costs far less GPU time, but moving shadows update in small steps. Server Tick follows the server's tick rate."));
        screen->components.push_back(MakeNavButton("Done", [hub] { GUIController::OpenScreen(hub); }));
    }

    Screen* performance = CreateVideoCategory("Performance");
    {
        Screen* screen = performance;
    screen->components.push_back(WithTooltip(MakeSlider(config, "Max FPS", "Graphics.TargetFPS", 30, 360, 60, "",
        [state](int32_t value) { state->targetFPS = static_cast<uint16_t>(value); }),
        "Frame rate limit. With VSync on, your monitor's refresh rate also caps it."));
    screen->components.push_back(WithTooltip(MakeToggle(config, "VSync", "Graphics.VSync", true),
        "Syncs frames to your monitor to prevent tearing. Off gives the lowest input lag."));
    screen->components.push_back(WithTooltip(new OptionButton("Buffering", { "Double", "Triple" },
        [config] { return std::get<uint32_t>(config->Get("Graphics.BufferCount", uint32_t{2})) >= 3 ? size_t{1} : size_t{0}; },
        [config](size_t index) { config->Set("Graphics.BufferCount", static_cast<uint32_t>(index + 2)); }),
        "Triple can smooth out frame pacing at the cost of slightly more input lag."));
    screen->components.push_back(WithTooltip(MakeToggle(config, "Indirect Draw", "Graphics.IndirectDraw", true),
        "Draws the world with far fewer GPU commands, which cuts CPU use. Turn off if you see missing or glitched terrain."));
    screen->components.push_back(WithTooltip(MakeToggle(config, "Debug Entity Tags", "Debug.EntityTags", false),
        "Shows a tag with the type and coordinates above every entity. Off shows only player names, like vanilla."));
    screen->components.push_back(WithTooltip(MakeToggle(config, "Entity Occlusion", "Graphics.EntityOcclusionCulling", false),
        "Skips mobs, items and nametags hidden behind terrain, which saves GPU time. Off by default while mobs vanishing is investigated."));
        screen->components.push_back(MakeNavButton("Done", [hub] { GUIController::OpenScreen(hub); }));
    }

    hub->components.push_back(MakeNavButton("Display", [display] { GUIController::OpenScreen(display); }));
    hub->components.push_back(MakeNavButton("Quality", [quality] { GUIController::OpenScreen(quality); }));
    hub->components.push_back(MakeNavButton("Shadows", [shadows] { GUIController::OpenScreen(shadows); }));
    hub->components.push_back(MakeNavButton("Performance", [performance] { GUIController::OpenScreen(performance); }));
    hub->components.push_back(MakeNavButton("Done", [settingsScreen] { GUIController::OpenScreen(settingsScreen); }));
    return hub;
}

Screen* CreateWindowScreen(GlobalState* state, Screen* settingsScreen)
{
    Config* config = state->config;
    Screen* screen = GUIController::CreateScreen("Window Settings", false);
    screen->components.push_back(WithTooltip(new OptionButton("Mode", { "Windowed", "Borderless", "Fullscreen" },
        [config] { return static_cast<size_t>(std::get<uint32_t>(config->Get("Window.Mode", uint32_t{0}))); },
        [config](size_t index) { config->Set("Window.Mode", static_cast<uint32_t>(index)); }),
        "Fullscreen takes exclusive control of the display for the lowest input lag. Borderless switches apps instantly."));
    screen->components.push_back(WithTooltip(MakeFullscreenModeOption(config),
        "Resolution and refresh rate used in Fullscreen mode. Desktop keeps your current display mode."));
    screen->components.push_back(WithTooltip(MakeNumber(config, "Windowed Width", "Window.Width", 320, 7680, 854),
        "Window width in Windowed mode. Press Enter to apply."));
    screen->components.push_back(WithTooltip(MakeNumber(config, "Windowed Height", "Window.Height", 240, 4320, 480),
        "Window height in Windowed mode. Press Enter to apply."));
    screen->components.push_back(MakeNavButton("Done", [settingsScreen] { GUIController::OpenScreen(settingsScreen); }));
    return screen;
}

Screen* CreateAudioScreen(GlobalState* state, Screen* settingsScreen)
{
    Config* config = state->config;
    Screen* screen = GUIController::CreateScreen("Audio Settings", false);
    screen->components.push_back(WithTooltip(MakeSlider(config, "Master Volume", "Audio.Volume", 0, 100, 100, "%"),
        "Overall volume of everything the client plays."));
    screen->components.push_back(WithTooltip(MakeSlider(config, "Music", "Audio.Music.Volume", 0, 100, 40, "%"),
        "Volume of the background music, on top of Master Volume. 0 silences it."));
    screen->components.push_back(MakeNavButton("Done", [settingsScreen] { GUIController::OpenScreen(settingsScreen); }));
    return screen;
}

Screen* CreateControlsScreen(GlobalState* state, Screen* settingsScreen)
{
    Config* config = state->config;
    Screen* screen = GUIController::CreateScreen("Controls", false);
    Screen* keyBindsScreen = nullptr;
    screen->components.push_back(WithTooltip(MakeSlider(config, "Mouse Sensitivity", "Controls.Mouse.Sensitivity", 10, 300, 100, "%"),
        "How far the camera turns per mouse movement."));
    screen->components.push_back(WithTooltip(MakeToggle(config, "Raw Input", "Controls.Mouse.RawInput", true),
        "Reads the mouse directly, skipping Windows pointer acceleration, in one batch per frame. Takes effect after a restart."));
    screen->components.push_back(WithTooltip(MakeToggle(config, "Toggle Sprint", "Controls.ToggleSprint", false),
        "Press sprint once to start sprinting instead of holding it."));
    keyBindsScreen = CreateKeyBindsScreen(state, [screen] { GUIController::OpenScreen(screen); });
    screen->components.push_back(WithTooltip(MakeNavButton("Key Binds", [keyBindsScreen] { GUIController::OpenScreen(keyBindsScreen); }),
        "Change the key or mouse button for movement, hotbar slots, dropping items and more."));
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
    Screen* audioScreen = CreateAudioScreen(state, screen);
    Screen* controlsScreen = CreateControlsScreen(state, screen);
    Screen* resourcePacksScreen = CreateResourcePacksScreen(state, [screen] { GUIController::OpenScreen(screen); });

    screen->components.push_back(MakeNavButton("Video", [videoScreen] { GUIController::OpenScreen(videoScreen); }));
    screen->components.push_back(MakeNavButton("Window", [windowScreen] { GUIController::OpenScreen(windowScreen); }));
    screen->components.push_back(MakeNavButton("Audio", [audioScreen] { GUIController::OpenScreen(audioScreen); }));
    screen->components.push_back(MakeNavButton("Controls", [controlsScreen] { GUIController::OpenScreen(controlsScreen); }));
    screen->components.push_back(MakeNavButton("Resource Packs", [resourcePacksScreen] { GUIController::OpenScreen(resourcePacksScreen); }));
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
