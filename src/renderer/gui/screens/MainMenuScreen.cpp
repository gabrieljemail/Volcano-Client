#include <functional>
#include "MainMenuScreen.hpp"
#include "SettingsScreen.hpp"
#include "../GUIController.hpp"
#include "../models/Button.hpp"
#include "../models/GUIComponent.hpp"

namespace Volcano::GUI {

Screen* CreateMainMenuScreen(GlobalState* state, Screen* singleplayerScreen, Screen* multiplayerScreen,
    Screen* settingsScreen)
{
    Screen* mainMenuScreen = GUIController::CreateScreen("Volcano Client", /* closable */ false);
    mainMenuScreen->showLogo = true;

    auto* singleplayerButton = new Button("Singleplayer");
    singleplayerButton->AddClickHandler(new std::function<void()>(
        [singleplayerScreen] { GUIController::OpenScreen(singleplayerScreen); }));

    auto* multiplayerButton = new Button("Multiplayer");
    multiplayerButton->AddClickHandler(new std::function<void()>(
        [multiplayerScreen] { GUIController::OpenScreen(multiplayerScreen); }));

    auto* settingsButton = new Button("Settings");
    settingsButton->AddClickHandler(new std::function<void()>(
        [settingsScreen, mainMenuScreen] { OpenSettingsScreen(settingsScreen, mainMenuScreen); }));

    auto* quitButton = new Button("Quit Game");
    quitButton->AddClickHandler(new std::function<void()>([state] { state->shouldClose = true; }));

    mainMenuScreen->components.push_back(singleplayerButton);
    mainMenuScreen->components.push_back(multiplayerButton);
    mainMenuScreen->components.push_back(settingsButton);
    mainMenuScreen->components.push_back(quitButton);

    return mainMenuScreen;
}

} // namespace Volcano::GUI
