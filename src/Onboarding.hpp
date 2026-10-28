#pragma once
#ifndef VOLCANO_ONBOARDING_H
#define VOLCANO_ONBOARDING_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <variant>
#include <vector>
#include "Config.hpp"
#include "GlobalState.hpp"
#include "KeyBindings.hpp"
#include "renderer/gui/GUIController.hpp"
#include "renderer/gui/models/Button.hpp"
#include "renderer/gui/models/OptionButton.hpp"
#include "renderer/gui/models/Screen.hpp"
#include "renderer/gui/models/Slider.hpp"
#include "renderer/gui/screens/KeyBindsScreen.hpp"

namespace Volcano {

/**
 * Manages an introduction process with multiple screens.
 * This process asks the user to set up the following settings:
 * - FOV
 * - GUI Scale
 * - Gamma
 * - Shadow Preset
 * --- Next Page ---
 * - Master Volume
 * - Music Volume
 * --- Next Page ---
 * - Keybinds
 * ---   Done!   ---
 *
 * Every change applies at once, like the Settings screens. The first launch is over once the last page's Finish
 * (or Skip Setup) is pressed, and the title screen opens then; on any later launch the title screen opens straight
 * away. The screens call back into this object, so keep it alive as long as the client runs.
 */
class Onboarding {
public:
    Onboarding(GlobalState* globalState, GUI::Screen* mainMenu)
        : state(globalState), titleScreen(mainMenu)
    {
        bool firstLaunch = std::get<bool>(state->config->Get("UserState.FirstLaunch", true));
        if (!firstLaunch)
        {
            OpenTitleScreen();
            return;
        }

        pages = { BuildWelcomePage(), BuildGraphicsPage(), BuildAudioPage(), BuildKeybindsPage() };
        ShowPage(0);
    }

    Onboarding(const Onboarding&) = delete;
    Onboarding& operator=(const Onboarding&) = delete;

private:
    // The shadow settings each preset stands for.
    struct ShadowPreset {
        bool enabled;
        uint32_t resolution;    // Index into Shadow Resolution: 512, 1024, 2048, 4096.
        uint32_t distance;      // Blocks.
    };
    static constexpr ShadowPreset SHADOW_PRESETS[] = {
        { false, 1, 128 },      // Off
        { true, 0, 64 },        // Low
        { true, 1, 128 },       // Medium
        { true, 2, 256 },       // High
    };

    GlobalState* state;
    GUI::Screen* titleScreen;
    std::vector<GUI::Screen*> pages;
    size_t page = 0;
    GUI::Screen* keyBindsScreen = nullptr;  // Made the first time it is asked for.

    void ShowPage(size_t index)
    {
        page = index;
        GUIController::OpenScreen(pages[page]);
    }

    void NextPage()
    {
        if (page + 1 < pages.size()) ShowPage(page + 1);
        else Finish();
    }

    void PreviousPage()
    {
        if (page > 0) ShowPage(page - 1);
    }

    void OpenTitleScreen()
    {
        if (titleScreen != nullptr) GUIController::OpenScreen(titleScreen);
    }

    // Onboarding is over for good, however it ended.
    void Finish()
    {
        state->config->Set("UserState.FirstLaunch", false);
        OpenTitleScreen();
    }

    // ---- Components ----

    static GUI::Button* MakeButton(const std::string& label, std::function<void()> onClick)
    {
        auto* button = new GUI::Button(label);
        button->AddClickHandler(new std::function<void()>(std::move(onClick)));
        return button;
    }

    static GUI::GUIComponent* MakeText(const std::string& text)
    {
        return new GUI::GUIComponent(GUI::GUIComponentType::TEXT, text);
    }

    template<typename T>
    static T* WithTooltip(T* component, const std::string& text)
    {
        component->SetTooltip(text);
        return component;
    }

    GUI::Slider* MakeSlider(const std::string& label, const std::string& key, int32_t min, int32_t max,
        uint32_t fallback, const std::string& suffix = "")
    {
        Config* config = state->config;
        return new GUI::Slider(label, min, max,
            [config, key, fallback] { return static_cast<int32_t>(std::get<uint32_t>(config->Get(key, fallback))); },
            [config, key](int32_t value) { config->Set(key, static_cast<uint32_t>(value)); },
            suffix);
    }

    // Next, then Back, under a page's settings. The welcome page has its own buttons.
    void AddNavigation(GUI::Screen* screen, const std::string& nextLabel)
    {
        screen->components.push_back(MakeButton(nextLabel, [this] { NextPage(); }));
        screen->components.push_back(MakeButton("Back", [this] { PreviousPage(); }));
    }

    // ---- Pages ----

    GUI::Screen* BuildWelcomePage()
    {
        GUI::Screen* screen = GUIController::CreateScreen("Welcome to Volcano Client!", false);
        screen->components.push_back(MakeText("Let's set a few things up before you play."));
        screen->components.push_back(MakeText("You can change all of them later in Settings."));
        screen->components.push_back(MakeButton("Get Started", [this] { NextPage(); }));
        screen->components.push_back(MakeButton("Skip Setup", [this] { Finish(); }));
        return screen;
    }

    GUI::Screen* BuildGraphicsPage()
    {
        Config* config = state->config;
        GUI::Screen* screen = GUIController::CreateScreen("How should things look?", false);

        screen->components.push_back(WithTooltip(MakeSlider("FOV", "Graphics.FOV", 30, 110, 100),
            "Field of view in degrees. Higher shows more around you; lower zooms in."));

        screen->components.push_back(WithTooltip(new GUI::OptionButton("GUI Scale", { "Auto", "1x", "2x", "3x", "4x" },
            [config] { return static_cast<size_t>(std::min<uint32_t>(std::get<uint32_t>(config->Get("Graphics.GUIScale", uint32_t{0})), 4)); },
            [config](size_t index) { config->Set("Graphics.GUIScale", static_cast<uint32_t>(index)); }),
            "How big menus and the HUD are. Auto picks one for your screen."));

        screen->components.push_back(WithTooltip(MakeSlider("Gamma", "Graphics.Brightness", 0, 100, 50, "%"),
            "Brightens or darkens the whole picture. Raise it if caves and nights are too dark to see in."));

        screen->components.push_back(WithTooltip(new GUI::OptionButton("Shadows", { "Off", "Low", "Medium", "High" },
            [config]
            {
                if (!std::get<bool>(config->Get("Graphics.Shadows.Enabled", false))) return size_t{0};
                uint32_t resolution = std::get<uint32_t>(config->Get("Graphics.Shadows.Resolution", uint32_t{1}));
                return static_cast<size_t>(std::min<uint32_t>(resolution, 2) + 1);
            },
            [config](size_t index)
            {
                const ShadowPreset& preset = SHADOW_PRESETS[std::min<size_t>(index, std::size(SHADOW_PRESETS) - 1)];
                config->Set("Graphics.Shadows.Enabled", preset.enabled);
                if (!preset.enabled) return; // Leave the size and distance as they were.
                config->Set("Graphics.Shadows.Resolution", preset.resolution);
                config->Set("Graphics.Shadows.Distance", preset.distance);
            }),
            "Sun shadows from terrain, plants and mobs. Experimental, and the higher presets cost a lot of GPU time. Fine-tune it in Settings."));

        AddNavigation(screen, "Next");
        return screen;
    }

    GUI::Screen* BuildAudioPage()
    {
        GUI::Screen* screen = GUIController::CreateScreen("How loud should things be?", false);
        screen->components.push_back(WithTooltip(MakeSlider("Master Volume", "Audio.Volume", 0, 100, 100, "%"),
            "Overall volume of everything the client plays."));
        screen->components.push_back(WithTooltip(MakeSlider("Music", "Audio.Music.Volume", 0, 100, 40, "%"),
            "Volume of the background music, on top of Master Volume. 0 silences it."));
        AddNavigation(screen, "Next");
        return screen;
    }

    GUI::Screen* BuildKeybindsPage()
    {
        GUI::Screen* screen = GUIController::CreateScreen("What keybinds do you like better?", false);
        screen->components.push_back(MakeText("The defaults match Minecraft's. Change any of them if you like."));
        screen->components.push_back(MakeButton("Customize Keybinds", [this]
        {
            if (keyBindsScreen == nullptr) keyBindsScreen = GUI::CreateKeyBindsScreen(state, [this] { ShowPage(page); });
            GUIController::OpenScreen(keyBindsScreen);
        }));
        screen->components.push_back(MakeButton("Reset to Defaults", [this] { KeyBindings::ResetAll(*state->config, *state->input); }));
        AddNavigation(screen, "Finish");
        return screen;
    }
};

} // namespace Volcano

#endif
