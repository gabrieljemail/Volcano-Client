#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <stop_token>
#include <GLFW/glfw3.h>
#include "CrashReporter.hpp"
#include "GlobalState.hpp"
#include "RideController.hpp"
#include "ModLoader.hpp"
#include "InputHandler.hpp"
#include "Logger.hpp"
#include "resources/BaseAssets.hpp"
#include "resources/ResourceManager.hpp"
#include "resources/ResourceReloader.hpp"
#include "platform/OS.hpp"
#include "audio/SoundRegistry.hpp"
#include "audio/BlockSounds.hpp"
#include "audio/EntitySounds.hpp"
#include "renderer/terrain/LightEngine.hpp"
#include "audio/MusicController.hpp"
#include "renderer/misc/ItemRenderer.hpp"
#include "renderer/particle/ParticleSystem.hpp"
#include "renderer/RenderThread.hpp"
#include "renderer/engine/vulkan/VulkanRenderEngine.hpp"
#include "renderer/SplashScreen.hpp"
#include "renderer/TextureManager.hpp"
#include "renderer/terrain/models/Chunk.hpp"
#include "renderer/terrain/models/World.hpp"
#include "renderer/terrain/models/BlockRegistry.hpp"
#include "renderer/entity/models/EntityRegistry.hpp"
#include "inventory/ItemRegistry.hpp"
#include "renderer/terrain/ChunkMesher.hpp"
#include "renderer/terrain/MeshingThread.hpp"
#include "renderer/gui/GUIController.hpp"
#include "renderer/gui/models/Screen.hpp"
#include "KeyBindings.hpp"
#include "Onboarding.hpp"
#include "renderer/gui/screens/LoadingScreen.hpp"
#include "renderer/gui/screens/MainMenuScreen.hpp"
#include "renderer/gui/screens/PauseScreen.hpp"
#include "renderer/gui/screens/ServerList.hpp"
#include "renderer/gui/screens/SettingsScreen.hpp"
#include "renderer/gui/models/TextInput.hpp"
#include "renderer/gui/models/Button.hpp"
#include "network/NetworkThread.hpp"
#include "PlayerAttributes.hpp"
#include "TickLoop.hpp"
#include "TickThread.hpp"
#include "interaction/InteractionManager.hpp"
#include "interaction/CombatSounds.hpp"
#include "audio/AudioManager.hpp"
#include <filesystem>
#include <vector>
using namespace std;

// Defaults for the connect screen's fields are read from Network.DevServerHost/
// Network.DevServerPort/Network.DevUsername (settings.json) - this is the
// developer's own test server, prefilled purely for convenience; both
// fields stay editable in the UI.

// Splits "host:port" into its parts, falling back to defaultPort if no
// ':' is present (so plain hostnames/IPs still work).
static void ParseServerAddress(const std::string& address, std::string& outHost, uint16_t& outPort, uint16_t defaultPort)
{
    size_t colon = address.rfind(':');
    if (colon == std::string::npos)
    {
        outHost = address;
        outPort = defaultPort;
        return;
    }

    outHost = address.substr(0, colon);
    try {
        outPort = static_cast<uint16_t>(std::stoi(address.substr(colon + 1)));
    } catch (const std::exception&) {
        outPort = defaultPort;
    }
}

int main(int argc, char** argv)
{
    Volcano::Platform::OS::AttachParentConsole();
    Volcano::CrashReporter::Install();
    if (Volcano::CrashReporter::HandleReportMode(argc, argv)) return 0;

    // Let's keep this just for fun.
    Volcano::Log::Info("Hello, World!");

    // Cool terminal effects.
    std::cout << "\x1b[2J\x1b[H"; // Clear the screen and reset the cursor.
    std::cout << "\x1b]0;Volcano Client Log\x07"; // Title.

    // Initialize global state.
    Volcano::GlobalState state{};
    state.LoadSettings();

    // Resource packs must exist before any asset read.
    Volcano::ResourceManager resourceManager(&state);
    state.resourceManager = &resourceManager;

    // Performance.HighPriority: run one step above normal so other apps
    // compete less with the render thread on low-core machines. Set before
    // any thread is created so every thread inherits it.
    if (std::get<bool>(state.config->Get("Performance.HighPriority", true)))
    {
        if (!Volcano::Platform::OS::SetAboveNormalPriority())
        {
            Volcano::Log::Info("Couldn't raise process priority; running at normal priority.");
        }
    }

    Volcano::ModLoader modLoader(&state);
    state.modLoader = &modLoader;
    modLoader.Init();
    modLoader.Fire(Volcano::EventType::PRE_START);

    // No Vulkan/GLFW dependency. Save this early since the debug overlay
    // wants it available from the start.
    state.systemInfo.DetectCPU();

    Volcano::Engine::Vulkan::VulkanRenderEngine renderEngine(&state);
    state.renderEngine = &renderEngine;
    renderEngine.Init();

    Volcano::RenderThread renderer(&state);
    Volcano::SplashScreen::Show();
    modLoader.Fire(Volcano::EventType::SPLASH_SCREEN);

    // First run: the game's own assets aren't shipped, so ask, then fetch them from Mojang before anything reads them.
    if (!Volcano::BaseAssets::Installed())
    {
        std::string failure;
        bool agreed = Volcano::Platform::OS::ShowConsentDialog("Download game assets", Volcano::BaseAssets::NOTICE,
            Volcano::BaseAssets::AGREE_LABEL);
        bool ready = agreed && Volcano::BaseAssets::Download(
            [](float progress, const std::string&)
            {
                Volcano::SplashScreen::SetProgress(progress * 0.98f);
            },
            failure);
        if (!ready)
        {
            if (agreed) Volcano::Platform::OS::ShowErrorDialog("Download failed", failure);
            Volcano::SplashScreen::Hide();
            return 1;
        }
        resourceManager.Reload();
    }

    // Each init step below reports progress, which also pumps the window's
    // event queue so the OS doesn't mark the client "Not Responding".
    Volcano::SplashScreen::SetProgress(0.02f);

    // Audio comes after the window/splash so the slow sound indexing and
    // decoding below doesn't leave the user staring at nothing.
    Volcano::AudioManager audioManager;
    state.audioManager = &audioManager;
    audioManager.Init();
    Volcano::SplashScreen::SetProgress(0.05f);

    Volcano::ParticleSystem particleSystem;
    state.particles = &particleSystem;
    particleSystem.Init();

    Volcano::SoundRegistry soundRegistry("assets/minecraft/sounds");
    soundRegistry.IndexSounds();
    state.soundRegistry = &soundRegistry;
    Volcano::BlockSounds blockSounds(&state);
    state.blockSounds = &blockSounds;
    Volcano::EntitySounds entitySounds(&state);
    state.entitySounds = &entitySounds;
    Volcano::CombatSounds combatSounds(&state);
    state.combatSounds = &combatSounds;
    modLoader.Fire(Volcano::EventType::AUDIO_LOADED);
    Volcano::SplashScreen::SetProgress(0.10f);

    Volcano::MusicController musicController(&state, &soundRegistry);
    state.musicController = &musicController;

    // Create the player.
    Volcano::Player player;
    state.player = &player;

    // Input handler
    Volcano::InputHandler input(state.window);
    state.input = &input;
    if (std::get<bool>(state.config->Get("Controls.Mouse.RawInput", true))) input.EnableRawMouse();

    // Register camera inputs. Sensitivity (degrees per pixel of mouse motion)
    // is the ONLY scaling applied to mouse look
    input.RegisterAxis("Camera.X", InputAxis{ -1.0f, 1.0f, 0.0f, 0.15f, 0, 0, AxisSourceType::MouseX, false });
    input.RegisterAxis("Camera.Y", InputAxis{ -1.0f, 1.0f, 0.0f, 0.15f, 0, 0, AxisSourceType::MouseY, false });

    // Movement, actions and hotbar keys are rebindable (see KeyBindings.hpp):
    // the Move.* axes start with no keys and Apply fills them in from
    // settings.json's Controls.Keys.*, as it does every action. Movement is
    // client-side, matching Minecraft
    input.RegisterAxis("Move.Forward", InputAxis{ -1.0f, 1.0f, 0.0f, 1.0f, 0, 0, AxisSourceType::KeyPair, false });
    input.RegisterAxis("Move.Right", InputAxis{ -1.0f, 1.0f, 0.0f, 1.0f, 0, 0, AxisSourceType::KeyPair, false });
    Volcano::KeyBindings::Apply(*state.config, input);

    // Fixed: RELEASE so F3+Z (wireframe, see RenderThread::PollInputs) can
    // cancel it, which only works with F3 itself.
    input.RegisterAction("ToggleDebugHud", InputActionTriggerType::RELEASE, {GLFW_KEY_F3});

    // Builds the protocol block-state-id -> texture mapping from
    // minecraft-data + the vanilla blockstate/model JSON.
    Volcano::SplashScreen::SetProgress(0.12f);
    Volcano::BlockRegistry::Init();
    Volcano::SplashScreen::SetProgress(0.18f);
    Volcano::EntityRegistry::Init();
    Volcano::SplashScreen::SetProgress(0.22f);

    // Initialize texture manager and load textures. This is the longest
    // step, so it reports per-texture progress across 0.22 -> 0.85.
    Volcano::Log::Info("Loading textures... (this may take a moment)");
    Volcano::TextureManager textureManager;
    textureManager.LoadResourcePack([](float fraction) {
        Volcano::SplashScreen::SetProgress(0.22f + fraction * 0.63f);
    });
    Volcano::SplashScreen::SetProgress(0.85f);
    Volcano::Log::Info("Texture loading complete!");
    modLoader.Fire(Volcano::EventType::TEXTURES_LOADED);

    // Resolves item icon texture layers against the array textureManager
    // just built
    Volcano::ItemRegistry::Init(textureManager);
    Volcano::SplashScreen::SetProgress(0.88f);

    // Point the world passes at the texture array, and start the ImGui
    // backend against the engine's render pass.
    renderEngine.AttachTextures(textureManager);
    Volcano::SplashScreen::SetProgress(0.92f);
    renderEngine.AttachGUI(&textureManager);
    Volcano::SplashScreen::SetProgress(0.95f);

    // Draws 3D item models (inventory icons for blocks and custom models).
    // Needs the texture array attached above.
    Volcano::ItemRenderer itemRenderer(&textureManager);
    Volcano::Engine::ShaderLibrary itemShaders;
    itemRenderer.Init(itemShaders);
    state.itemRenderer = &itemRenderer;
    Volcano::SplashScreen::SetProgress(0.97f);

    // World (placeholder - see the tick-loop plan) and attributes must exist
    // before GenerateChunks/TickLoop touch them.
    Volcano::World world;
    state.world = &world;
    Volcano::LightEngine lightEngine(&world);
    state.lightEngine = &lightEngine;

    Volcano::PlayerAttributes attributes = Volcano::PlayerAttributes::Defaults();
    state.attributes = &attributes;

    Volcano::InteractionManager interaction(&state);
    state.interaction = &interaction;

    // Constructed after player/input/world/attributes so its constructor
    // (which reads the player's starting position) has everything it needs.
    Volcano::TickLoop tickLoop(&state);
    state.tickLoop = &tickLoop;
    Volcano::RideController rideController(&state);
    state.rideController = &rideController;

    Volcano::MeshingThread meshingThread(&state, &textureManager);
    meshingThread.Start();

    // F3+T / settings GUI reload (see ResourceReloader.hpp). Startup above
    // keeps calling the same per-subsystem Init functions in its own order.
    Volcano::ResourceReloader::Attach(&textureManager, &meshingThread);

    Volcano::TickThread tickThread(&state);
    tickThread.Start();

    std::unique_ptr<Volcano::NetworkThread> network;

    // Singleplayer screen: one button per settings.json's
    // Network.LocalServerDirs entry, each launching that directory's
    // server.jar on Network.LoopbackPort and connecting to it exactly like
    // any other server
    Volcano::GUI::Screen* singleplayerScreen = Volcano::GUIController::CreateScreen("Singleplayer", /* closable */ false);

    // Shown from the moment a connection attempt starts (singleplayer or
    // multiplayer) until the world is ready - the main loop below closes it
    // once GlobalState::worldReady flips. See LoadingScreen.hpp; what it
    // says follows GlobalState::connectionPhase. Cancel (only offered while
    // a local server is starting) doesn't block on the server shutting down
    // - see RequestStop's own comment. The old NetworkThread (and its
    // still-shutting-down LocalServer) gets joined for real whenever
    // `network` is next reassigned, not here, so this can't freeze the
    // render thread waiting on LocalServer::Stop()'s up-to-30s grace period.
    Volcano::GUI::Screen* loadingScreen = Volcano::GUI::CreateLoadingScreen(&state,
        [&state, &network, singleplayerScreen] {
            if (network) network->RequestStop();
            state.SetConnectionPhase(Volcano::ConnectionPhase::Idle);
            Volcano::GUIController::OpenScreen(singleplayerScreen);
        });

    // Connect screen: collects server address + username and starts the
    // network thread on "Connect". Shown immediately so the player never
    // sees a hardcoded server.
    Volcano::GUI::Screen* connectScreen = Volcano::GUIController::CreateScreen("Connect to Server", /* closable */ false);

    uint16_t devServerPort = static_cast<uint16_t>(std::get<uint32_t>(state.config->Get("Network.DevServerPort", uint32_t{25565})));
    auto* addressInput = new Volcano::GUI::TextInput(
        "Server Address",
        std::get<std::string>(state.config->Get("Network.DevServerHost", std::string("108.197.182.119"))) + ":" + std::to_string(devServerPort));
    auto* usernameInput = new Volcano::GUI::TextInput(
        "Username", std::get<std::string>(state.config->Get("Network.DevUsername", std::string("VoidDev"))));
    auto* connectButton = new Volcano::GUI::Button("Connect");

    // Shown above the form after a disconnect (see the polling loop below);
    // empty/invisible on the very first, pre-connection showing of this
    // screen.
    auto* disconnectReasonText = new Volcano::GUI::GUIComponent(Volcano::GUI::GUIComponentType::TEXT, "");
    disconnectReasonText->SetVisibile(false);

    // Starts a connection to "host[:port]" as the username on the connect
    // screen: shared by its Connect button and the server list's Join.
    auto connectToServer = [&state, &network, usernameInput, loadingScreen, devServerPort](const std::string& address) {
        std::string host;
        uint16_t port;
        ParseServerAddress(address, host, port, devServerPort);
        std::string username = usernameInput->GetValue();
        if (host.empty() || username.empty()) return;

        state.SetConnectionPhase(Volcano::ConnectionPhase::Connecting, host + ":" + std::to_string(port));
        network = std::make_unique<Volcano::NetworkThread>(&state, host, port, username);
        network->Start();

        Volcano::GUIController::OpenScreen(loadingScreen);
    };
    connectButton->AddClickHandler(new std::function<void()>([connectToServer, addressInput] {
        connectToServer(addressInput->GetValue());
    }));

    // Built once here (the LocalServerDirs list isn't expected to change at
    // runtime) rather than re-scanning it on every screen open.
    uint16_t loopbackPort = static_cast<uint16_t>(std::get<uint32_t>(state.config->Get("Network.LoopbackPort", uint32_t{25565})));

    for (const std::string& serverDir : state.config->GetStringArray("Network.LocalServerDirs"))
    {
        if (!std::filesystem::exists(std::filesystem::path(serverDir) / "server.jar"))
        {
            Volcano::Log::Error("Singleplayer: no server.jar in " + serverDir + ", skipping");
            continue;
        }

        std::string worldName = std::filesystem::path(serverDir).filename().string();
        auto* worldButton = new Volcano::GUI::Button(worldName);
        worldButton->AddClickHandler(new std::function<void()>(
            [&state, &network, usernameInput, serverDir, loopbackPort, loadingScreen] {
                std::string username = usernameInput->GetValue();
                if (username.empty()) return;

                state.SetConnectionPhase(Volcano::ConnectionPhase::StartingServer);
                network = std::make_unique<Volcano::NetworkThread>(&state, "127.0.0.1", loopbackPort, username, serverDir);
                network->Start();

                Volcano::GUIController::OpenScreen(loadingScreen);
            }));
        singleplayerScreen->components.push_back(worldButton);
    }

    connectScreen->components.push_back(disconnectReasonText);
    connectScreen->components.push_back(addressInput);
    connectScreen->components.push_back(usernameInput);
    connectScreen->components.push_back(connectButton);

    // Main menu: the actual startup screen and navigation hub now
    Volcano::GUI::Screen* settingsScreen = Volcano::GUI::CreateSettingsScreen(&state);
    // Multiplayer opens the saved-server list; Direct Connect from there opens
    // connectScreen. Its Back needs the main menu, which is built just after.
    Volcano::GUI::Screen* mainMenuScreen = nullptr;
    Volcano::GUI::ServerListActions serverListActions;
    serverListActions.join = [connectToServer](const Volcano::GUI::ServerEntry& server) { connectToServer(server.address); };
    serverListActions.directConnect = [connectScreen] { Volcano::GUIController::OpenScreen(connectScreen); };
    serverListActions.back = [&mainMenuScreen] { Volcano::GUIController::OpenScreen(mainMenuScreen); };
    Volcano::GUI::Screen* serverListScreen = Volcano::GUI::CreateServerList(serverListActions);

    mainMenuScreen = Volcano::GUI::CreateMainMenuScreen(&state, singleplayerScreen, serverListScreen, settingsScreen);

    // Pause menu (Esc in a world). Servers and Disconnect both leave the
    // current session the same way
    Volcano::GUI::Screen* disconnectDestination = nullptr;
    auto beginDisconnect = [&state, &network, &disconnectDestination, loadingScreen](Volcano::GUI::Screen* destination) {
        disconnectDestination = destination;
        state.SetConnectionPhase(Volcano::ConnectionPhase::Disconnecting);
        if (network) network->RequestStop();
        Volcano::GUIController::OpenScreen(loadingScreen);
    };
    Volcano::GUIController::SetPauseScreen(Volcano::GUI::CreatePauseScreen(
        [beginDisconnect, serverListScreen] { beginDisconnect(serverListScreen); },
        [settingsScreen] { Volcano::GUI::OpenSettingsScreen(settingsScreen, Volcano::GUIController::GetPauseScreen()); },
        [beginDisconnect, mainMenuScreen] { beginDisconnect(mainMenuScreen); }));

    auto* singleplayerBackButton = new Volcano::GUI::Button("Back");
    singleplayerBackButton->AddClickHandler(new std::function<void()>(
        [mainMenuScreen] { Volcano::GUIController::OpenScreen(mainMenuScreen); }));
    singleplayerScreen->components.push_back(singleplayerBackButton);

    auto* connectBackButton = new Volcano::GUI::Button("Back");
    connectBackButton->AddClickHandler(new std::function<void()>(
        [serverListScreen] { Volcano::GUIController::OpenScreen(serverListScreen); }));
    connectScreen->components.push_back(connectBackButton);

    // Loading is done: show a full bar for one frame, then free the
    // splash's Vulkan objects before the real frame loop takes over.
    Volcano::SplashScreen::SetProgress(1.0f);
    Volcano::SplashScreen::Hide();

    // Onboarding screen: Shown when the user launches the client for the first time.
    // This class will show the main menu once onboarding is done or it shouldn't be shown.
    Volcano::Onboarding onboarding(&state, mainMenuScreen);
    modLoader.Fire(Volcano::EventType::RENDER_START);

    // This is now the frame loop itself (see RenderThread::RunFrame - it
    // calls glfwPollEvents() internally as the first step of PollInputs)
    // rather than a separate 1ms message-pump loop alongside a dedicated
    // render thread. Chunk/spawn handoff still happens entirely on
    // MeshingThread (see above).
    while (!state.shouldClose)
    {
        // GUIController and NotifyFramePresented run in this same loop now,
        // but GLFW still only guarantees glfwSetInputMode is safe from the
        // thread that created the window
        if (int mode = state.pendingCursorMode.exchange(-1); mode != -1)
        {
            glfwSetInputMode(state.window, GLFW_CURSOR, mode);
        }

        // The focus callback only fires on a change, so a window that was already focused when it appeared (or
        // when a screen closed) could be left with a free cursor until the user clicked away and back. Trust
        // GLFW's own answer: in game, focused, with no screen or chat open, the cursor is captured.
        {
            bool focused = glfwGetWindowAttrib(state.window, GLFW_FOCUSED) != 0;
            state.windowFocused.store(focused);
            if (focused && state.connectionPhase.load() == Volcano::ConnectionPhase::Playing
                && !Volcano::GUIController::IsScreenOpen() && !Volcano::GUIController::IsChatInputOpen()
                && glfwGetInputMode(state.window, GLFW_CURSOR) != GLFW_CURSOR_DISABLED)
            {
                glfwSetInputMode(state.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            }
        }

        // NetworkThread flags this when a session ends unexpectedly (kicked,
        // dropped, login failure) - see GlobalState::ReportDisconnect. Reset
        // world/session state so the next connection starts clean instead of
        // layering onto the previous one's leftovers, then show the connect
        // screen again with the reason.
        if (state.disconnected.exchange(false))
        {
            std::string reason;
            {
                std::lock_guard<std::mutex> lock(state.disconnectMutex);
                reason = state.disconnectReason;
            }
            Volcano::Log::Info("Returned to connect screen: " + reason);
            modLoader.Fire(Volcano::EventType::LEAVE_PLAY_STATE);

            // See GlobalState::ResetWorldState's own comment
            state.SetConnectionPhase(Volcano::ConnectionPhase::Idle);
            state.ResetWorldState();

            disconnectReasonText->SetLabel(reason);
            disconnectReasonText->SetVisibile(!reason.empty());
            disconnectDestination = nullptr; // The unexpected disconnect wins over a pending player-requested one.
            Volcano::GUIController::OpenScreen(connectScreen);
        }

        // Finishes a player-requested leave (pause menu's Servers/Disconnect
        // - see beginDisconnect above) once nothing can repopulate the world.
        if (disconnectDestination != nullptr && (!network || network->HasSessionEnded()))
        {
            Volcano::GUI::Screen* destination = disconnectDestination;
            disconnectDestination = nullptr;

            modLoader.Fire(Volcano::EventType::LEAVE_PLAY_STATE);
            state.SetConnectionPhase(Volcano::ConnectionPhase::Idle);
            state.ResetWorldState();

            disconnectReasonText->SetVisibile(false);
            Volcano::GUIController::OpenScreen(destination);
        }

        // The world actually streamed in (singleplayer or multiplayer alike)
        // - swap the loading screen out for the game itself. Checked after
        // the disconnect branch above (which already covers the failure
        // case: a connection dying before this ever becomes true reports
        // its own reason and reopens connectScreen instead, with the phase
        // back at Idle). Playing also keeps the menu panorama off across a
        // mid-session Respawn, where worldReady briefly drops again.
        if (state.connectionPhase.load() == Volcano::ConnectionPhase::LoadingTerrain && state.worldReady.load())
        {
            state.SetConnectionPhase(Volcano::ConnectionPhase::Playing);
            modLoader.Fire(Volcano::EventType::CHUNKS_READY);
            if (Volcano::GUIController::GetActiveScreen() == loadingScreen)
            {
                Volcano::GUIController::CloseScreen();
            }
        }

        musicController.Tick();

        renderer.RunFrame();
    }

    // Tear down the GUI and every render pass (waits for the GPU to finish
    // with them first) before renderEngine.Shutdown() below destroys the
    // device/window out from under them. No thread hand-off needed for this
    // anymore - RunFrame() above already stopped being called the moment
    // state.shouldClose went true, so nothing is still touching Vulkan/GLFW
    // by the time we get here. Previously nothing ever called
    // VulkanInit::Cleanup() at all: the process just exited with the Vulkan
    // device, swapchain, surface, instance and GLFW window all still alive,
    // which is what was hanging the graphics driver on shutdown (observed
    // on Windows/Intel Gen9) - the message-pump-while-stopping dance this
    // used to require was specifically to avoid that while a second thread
    // wound down; ThreadArchitecture.mdx's merge removes the second thread
    // instead of working around it.
    modLoader.Fire(Volcano::EventType::PRE_QUIT);
    renderer.Shutdown();

    // MeshingThread must be fully joined before renderEngine.Shutdown()
    // below - it calls ChunkMesher::Shutdown(), which destroys the slab
    // buffers MeshingThread writes into.
    meshingThread.Stop();

    itemRenderer.Shutdown();
    textureManager.Destroy();
    renderEngine.Shutdown();

    return 0;
}
