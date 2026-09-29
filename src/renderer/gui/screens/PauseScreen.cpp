#include "PauseScreen.hpp"
#include "../GUIController.hpp"
#include "../models/Button.hpp"

namespace Volcano::GUI {

Screen* CreatePauseScreen(std::function<void()> onServers,
                          std::function<void()> onSettings,
                          std::function<void()> onDisconnect)
{
    Screen* screen = GUIController::CreateScreen("Paused", /* closable */ true);

    auto* resumeButton = new Button("Resume");
    resumeButton->AddClickHandler(new std::function<void()>([] { GUIController::CloseScreen(); }));

    auto* serversButton = new Button("Servers");
    serversButton->AddClickHandler(new std::function<void()>(std::move(onServers)));

    auto* settingsButton = new Button("Settings");
    settingsButton->AddClickHandler(new std::function<void()>(std::move(onSettings)));

    auto* disconnectButton = new Button("Disconnect");
    disconnectButton->AddClickHandler(new std::function<void()>(std::move(onDisconnect)));

    screen->components.push_back(resumeButton);
    screen->components.push_back(serversButton);
    screen->components.push_back(settingsButton);
    screen->components.push_back(disconnectButton);
    return screen;
}

} // namespace Volcano::GUI
