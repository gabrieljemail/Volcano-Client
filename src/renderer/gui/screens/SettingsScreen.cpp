#include <functional>
#include "SettingsScreen.hpp"
#include "../GUIController.hpp"
#include "../models/Button.hpp"
#include "../models/GUIComponent.hpp"

namespace Volcano::GUI {

namespace {
Screen* returnScreen = nullptr;
}

Screen* CreateSettingsScreen()
{
    // Not closable: Esc closing it would leave no screen open at all when
    // it was reached from the main menu.
    Screen* screen = GUIController::CreateScreen("Settings", /* closable */ false);
    auto* comingSoonText = new GUIComponent(GUIComponentType::TEXT, "Coming soon.");
    auto* backButton = new Button("Back");
    backButton->AddClickHandler(new std::function<void()>([] {
        if (returnScreen != nullptr) GUIController::OpenScreen(returnScreen);
    }));
    screen->components.push_back(comingSoonText);
    screen->components.push_back(backButton);
    return screen;
}

void OpenSettingsScreen(Screen* settingsScreen, Screen* returnTo)
{
    returnScreen = returnTo;
    GUIController::OpenScreen(settingsScreen);
}

} // namespace Volcano::GUI
