#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <stop_token>
#include <GLFW/glfw3.h>
#include "CrashReporter.hpp"
#include "GlobalState.hpp"
#include "InputHandler.hpp"
#include "Logger.hpp"
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
#include "renderer/gui/screens/LoadingScreen.hpp"
#include "renderer/gui/screens/MainMenuScreen.hpp"
#include "renderer/gui/screens/PauseScreen.hpp"
#include "renderer/gui/screens/SettingsScreen.hpp"
#include "renderer/gui/models/TextInput.hpp"
#include "renderer/gui/models/Button.hpp"
#include "network/NetworkThread.hpp"
#include "PlayerAttributes.hpp"
#include "TickLoop.hpp"
#include "TickThread.hpp"
#include "interaction/InteractionManager.hpp"
#include <filesystem>
#include <vector>
using namespace std;

// Defaults for the connect screen's fields are read from Network.DevServerHost/
// Network.DevServerPort/Network.DevUsername (settings.json) — this is the
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

// Chunk generation function for 4-chunk view distance (2x2 grid)
void GenerateChunks(Volcano::GlobalState& state, const Volcano::TextureManager& textureManager) {
    Volcano::Log::Info("Generating 2x2 chunk grid...");

    // Generate 2x2 chunk grid (chunks 0 and 1 in both axes)
    for (int cx = 0; cx < 2; cx++) {
        for (int cz = 0; cz < 2; cz++) {
            // Built directly in World (not a local temporary) so TickLoop's
            // collision checks have real block data to query after this
            // function returns and the mesh-only view goes out of scope.
            Volcano::Chunk& chunk = state.world->GetOrCreateChunk(cx, cz);

            // Generate grass blocks for the top layer
            uint16_t stoneVisual = Volcano::BlockRegistry::VisualIdForName("stone");
            uint16_t dirtVisual = Volcano::BlockRegistry::VisualIdForName("dirt");
            uint16_t grassVisual = Volcano::BlockRegistry::VisualIdForName("grass_block");
            uint16_t logVisual = Volcano::BlockRegistry::VisualIdForName("oak_log");
            uint16_t leavesVisual = Volcano::BlockRegistry::VisualIdForName("oak_leaves");

            for (int x = 0; x < CHUNK_SIZE_X; x++) {
                for (int z = 0; z < CHUNK_SIZE_Z; z++) {
                    // Grass at y=1-3, dirt below, stone deeper
                    chunk.setBlock(x, 0, z, stoneVisual);
                    chunk.setBlock(x, 1, z, dirtVisual);
                    chunk.setBlock(x, 2, z, dirtVisual);
                    chunk.setBlock(x, 3, z, grassVisual);
                }
            }

            // Add some variation - random trees
            if (cx % 2 == 0 && cz % 2 == 0) {
                int treeX = 7 + (cx * 7) % 9;
                int treeZ = 7 + (cz * 7) % 9;
                // Simple tree: log + leaves
                for (int y = 4; y <= 6; y++) {
                    chunk.setBlock(treeX, y, treeZ, logVisual);
                }
                // Leaves around the top
                for (int lx = -1; lx <= 1; lx++) {
                    for (int lz = -1; lz <= 1; lz++) {
                        if (lx != 0 || lz != 0) {
                            chunk.setBlock(treeX + lx, 6, treeZ + lz, leavesVisual);
                        }
                    }
                }
                chunk.setBlock(treeX, 7, treeZ, leavesVisual);
            }

            // One mesh per non-empty 16-tall section now (see
            // ChunkMesher::MeshChunkSection) instead of one whole-column
            // mesh — this debug generator only ever touches y=0-7, so only
            // section 0 has anything to draw.
            for (int sectionY = 0; sectionY < Volcano::SECTIONS_PER_CHUNK; sectionY++) {
                if (chunk.IsSectionEmpty(sectionY)) continue;
                Volcano::Mesh chunkMesh = Volcano::ChunkMesher::MeshChunkSection(chunk, *state.world, textureManager, sectionY);

                // This debug generator ignores WORLD_MIN_Y (its blocks are
                // placed as if y=0 were the world floor, not -64) — override
                // MeshChunkSection's own model matrix the same way the old
                // whole-column call here always did.
                chunkMesh.modelMatrix = glm::translate(glm::mat4(1.0f),
                    glm::vec3(cx * CHUNK_SIZE_X, sectionY * Volcano::SECTION_SIZE_Y, cz * CHUNK_SIZE_Z));

                state.renderList.insert_or_assign(Volcano::SectionKey{ cx, cz, sectionY }, chunkMesh);
            }
        }
    }

    Volcano::Log::Info("Generated " + std::to_string(state.renderList.size()) + " chunk section meshes");
}

int main(int argc, char** argv)
{
    Volcano::CrashReporter::Install();
    if (Volcano::CrashReporter::HandleReportMode(argc, argv)) return 0;

    // Let's keep this just for fun.
    Volcano::Log::Info("Hello, World!");

    // Initialize global state.
    Volcano::GlobalState state{};
    state.LoadSettings();

    // No Vulkan/GLFW dependency — safe this early, and the debug overlay
    // wants it available from the very first frame.
    state.systemInfo.DetectCPU();

    // The rendering engine — everything below reaches the renderer through
    // this (GlobalState::renderEngine), never through Vulkan directly. Init()
    // creates the window/device/swapchain and every built-in render pass.
    Volcano::Engine::Vulkan::VulkanRenderEngine renderEngine(&state);
    state.renderEngine = &renderEngine;
    renderEngine.Init();

    // Create the render thread (frame loop — see RunFrame below).
    Volcano::RenderThread renderer(&state);

    // Shows resources/splash-screen.jpeg for one frame before the slow,
    // synchronous startup work below (block/entity registries, texture
    // atlas — collectively the ~15s the window would otherwise sit there
    // frozen and undrawn) actually starts. Presents once and tears its own
    // resources back down immediately — the compositor keeps showing that
    // presented frame on its own, so nothing needs to stay alive or be
    // re-drawn while the loading below blocks this thread.
    Volcano::SplashScreen::Show();

    // Create the player.
    Volcano::Player player;
    // GenerateChunks below plants its one tree at world (7, *, 7), with
    // leaves spanning x/z 6-8 — (8, 5, 8) put the player's head inside that
    // canopy. Spawn further into the 2x2 (32x32) chunk grid instead, clear
    // of the tree, so movement/jumping aren't blocked by leaf collision.
    player.SetPosition(glm::vec3(20.0f, 5.0f, 20.0f)); // Elevated to see the terrain
    state.player = &player;

    // Input handler - must be created before render thread starts
    Volcano::InputHandler input(state.window);
    state.input = &input;
    if (std::get<bool>(state.config->Get("Controls.Mouse.RawInput", true))) input.EnableRawMouse();

    // Register camera inputs. Sensitivity (degrees per pixel of mouse motion)
    // is the ONLY scaling applied to mouse look — RenderThread::PollInputs
    // passes 1.0f into Camera::ApplyMouseDelta so there's a single knob here,
    // not two multiplied together.
    input.RegisterAxis("Camera.X", InputAxis{ -1.0f, 1.0f, 0.0f, 0.15f, 0, 0, AxisSourceType::MouseX, false });
    input.RegisterAxis("Camera.Y", InputAxis{ -1.0f, 1.0f, 0.0f, 0.15f, 0, 0, AxisSourceType::MouseY, false });

    // Register movement inputs (client-side movement, matching how Minecraft
    // handles player movement — the server only reconciles/corrects it).
    // KeyPair axes: value is +1 while the positive key is held, -1 for the
    // negative key, 0 if neither/both are held.
    input.RegisterAxis("Move.Forward", InputAxis{ -1.0f, 1.0f, 0.0f, 1.0f, GLFW_KEY_W, GLFW_KEY_S, AxisSourceType::KeyPair, false });
    input.RegisterAxis("Move.Right", InputAxis{ -1.0f, 1.0f, 0.0f, 1.0f, GLFW_KEY_D, GLFW_KEY_A, AxisSourceType::KeyPair, false });
    // Space is now a real jump (TickLoop applies gravity/collision) instead
    // of the old fly-cam's continuous vertical axis. Two actions on the same
    // key: "Jump" (PRESS) drives TickLoop's edge-latch so a tap landing
    // between fixed ticks is never lost, and "JumpHold" (HOLD) lets it also
    // re-queue a jump on every frame the key is still down, which is what
    // gives holding Space vanilla's repeat-jump-on-landing behavior.
    input.RegisterAction("Jump", InputActionTriggerType::PRESS, {GLFW_KEY_SPACE});
    input.RegisterAction("JumpHold", InputActionTriggerType::HOLD, {GLFW_KEY_SPACE});
    input.RegisterAction("Sprint", InputActionTriggerType::HOLD, {GLFW_KEY_LEFT_CONTROL});
    input.RegisterAction("Sneak", InputActionTriggerType::HOLD, {GLFW_KEY_LEFT_SHIFT});
    input.RegisterAction("OpenChat", InputActionTriggerType::PRESS, {GLFW_KEY_T});
    input.RegisterAction("ToggleInventory", InputActionTriggerType::PRESS, {GLFW_KEY_E});
    input.RegisterAction("ToggleWireframe", InputActionTriggerType::PRESS, {GLFW_KEY_F3});
    input.RegisterAction("ToggleLighting", InputActionTriggerType::PRESS, {GLFW_KEY_G});
    input.RegisterAction("ReloadShaders", InputActionTriggerType::PRESS, {GLFW_KEY_F9});

    // Hotbar slot selection — vanilla's number-row bindings, key "1" for
    // slot 0 through "9" for slot 8. One PRESS action per key rather than a
    // single action with all nine triggers, since RenderThread::PollInputs
    // needs to know WHICH slot was pressed, not just that some hotbar key
    // was. GLFW_KEY_1..GLFW_KEY_9 are consecutive codepoints, so this can
    // register all nine off one loop instead of nine near-identical lines.
    for (int slot = 0; slot < 9; slot++)
    {
        input.RegisterAction("Hotbar." + std::to_string(slot),
            InputActionTriggerType::PRESS, {static_cast<uint16_t>(GLFW_KEY_1 + slot)});
    }

    // Mainhand/offhand interaction — read by InteractionManager::Update.
    // Primary gets a PRESS and a HOLD action on the same button, like
    // Jump/JumpHold: attacking only ever happens on the click itself
    // (vanilla doesn't keep attacking while the button is held), while
    // holding is what block breaking will need once it exists.
    input.RegisterAction("PrimaryActionPress", InputActionTriggerType::PRESS, {MOUSE_BUTTON_LEFT});
    input.RegisterAction("PrimaryAction", InputActionTriggerType::HOLD, {MOUSE_BUTTON_LEFT});

    // Secondary gets three actions on the same button — same pattern as
    // Jump/JumpHold above — since "use item" isn't just a click: an instant
    // item (a block, a bucket) only needs the press, but a continuous one
    // (food, a shield, a bow) needs to know it's still held, and that it's
    // been released, to know when to stop.
    input.RegisterAction("SecondaryAction", InputActionTriggerType::PRESS, {MOUSE_BUTTON_RIGHT});
    input.RegisterAction("SecondaryActionHold", InputActionTriggerType::HOLD, {MOUSE_BUTTON_RIGHT});
    input.RegisterAction("SecondaryActionRelease", InputActionTriggerType::RELEASE, {MOUSE_BUTTON_RIGHT});

    // Builds the protocol block-state-id -> texture mapping from
    // minecraft-data + the vanilla blockstate/model JSON. Must happen before
    // any chunk data is parsed or meshed (network/meshing threads start
    // further below).
    Volcano::BlockRegistry::Init();
    Volcano::EntityRegistry::Init();

    // Initialize texture manager and load textures
    Volcano::Log::Info("Loading textures... (this may take a moment)");
    Volcano::TextureManager textureManager;
    textureManager.LoadResourcePack("resources/minecraft");
    Volcano::Log::Info("Texture loading complete!");

    // Resolves item icon texture layers against the array textureManager
    // just built — must run after LoadResourcePack(), not alongside
    // BlockRegistry/EntityRegistry::Init() above (which only need JSON,
    // not the texture array).
    Volcano::ItemRegistry::Init(textureManager);

    // Point the world passes at the texture array, and start the ImGui
    // backend against the engine's render pass.
    renderEngine.AttachTextures(textureManager);
    renderEngine.AttachGUI(&textureManager);

    // World (placeholder — see the tick-loop plan) and attributes must exist
    // before GenerateChunks/TickLoop touch them.
    Volcano::World world;
    state.world = &world;

    Volcano::PlayerAttributes attributes = Volcano::PlayerAttributes::Defaults();
    state.attributes = &attributes;

    Volcano::InteractionManager interaction(&state);
    state.interaction = &interaction;

    // The world now loads from whatever server the player connects to (see
    // the "Connect" screen below) instead of a hardcoded grid. GenerateChunks
    // is kept above, unused, in case an offline/no-server mode is wanted
    // later.

    // Constructed after player/input/world/attributes so its constructor
    // (which reads the player's starting position) has everything it needs.
    Volcano::TickLoop tickLoop(&state);
    state.tickLoop = &tickLoop;

    // No renderer.Start() anymore — the frame loop runs inline in this
    // thread's own loop below (see the tick-loop plan's thread-merge step).

    // Meshing runs on its own thread too — greedy-meshing a full 16x384x16
    // chunk is real CPU work, and doing it inline on this thread (as it used
    // to) meant a burst of incoming chunks could stall the network thread's
    // own packet loop. See MeshingThread's header for why the work itself
    // doesn't need to run on any particular thread. Started immediately
    // (unlike NetworkThread) since it just idles until there's a connection.
    Volcano::MeshingThread meshingThread(&state, &textureManager);
    meshingThread.Start();

    // Drives TickLoop::Tick() and the per-tick position/rotation report on
    // its own steady cadence — see TickThread's own class comment for why
    // this can't just run inline on NetworkThread the way it used to.
    // Started immediately, same as MeshingThread just above: it idles
    // (worldReady stays false, GlobalState::activeConnection stays null)
    // until there's an actual connection to report to.
    Volcano::TickThread tickThread(&state);
    tickThread.Start();

    // Network runs on its own thread, deliberately separate from this
    // (GLFW-owning) main thread — see the networking plan for why. It's
    // only created once the player submits the connect screen below, since
    // the host/port/username aren't known until then.
    std::unique_ptr<Volcano::NetworkThread> network;

    // Singleplayer screen: one button per settings.json's
    // Network.LocalServerDirs entry, each launching that directory's
    // server.jar on Network.LoopbackPort and connecting to it exactly like
    // any other server — see NetworkThread's localServerDir constructor
    // argument. Its buttons are filled in further down.
    Volcano::GUI::Screen* singleplayerScreen = Volcano::GUIController::CreateScreen("Singleplayer", /* closable */ false);

    // Shown from the moment a connection attempt starts (singleplayer or
    // multiplayer) until the world is ready — the main loop below closes it
    // once GlobalState::worldReady flips. See LoadingScreen.hpp; what it
    // says follows GlobalState::connectionPhase. Cancel (only offered while
    // a local server is starting) doesn't block on the server shutting down
    // — see RequestStop's own comment. The old NetworkThread (and its
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

    connectButton->AddClickHandler(new std::function<void()>([&state, &network, addressInput, usernameInput, loadingScreen, devServerPort] {
        std::string host;
        uint16_t port;
        ParseServerAddress(addressInput->GetValue(), host, port, devServerPort);
        std::string username = usernameInput->GetValue();
        if (host.empty() || username.empty()) return;

        state.SetConnectionPhase(Volcano::ConnectionPhase::Connecting, host + ":" + std::to_string(port));
        network = std::make_unique<Volcano::NetworkThread>(&state, host, port, username);
        network->Start();

        Volcano::GUIController::OpenScreen(loadingScreen);
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

    // Main menu: the actual startup screen and navigation hub now — see
    // MainMenuScreen.hpp. Built after singleplayerScreen/connectScreen
    // (its Singleplayer/Multiplayer buttons open them directly), so their
    // own Back buttons (added right after, below) can close the loop back
    // to it instead of each other.
    Volcano::GUI::Screen* settingsScreen = Volcano::GUI::CreateSettingsScreen(&state);
    Volcano::GUI::Screen* mainMenuScreen =
        Volcano::GUI::CreateMainMenuScreen(&state, singleplayerScreen, connectScreen, settingsScreen);

    // Pause menu (Esc in a world). Servers and Disconnect both leave the
    // current session the same way — RequestStop, show the loading screen
    // as "Disconnecting...", and let the main loop below finish the job once
    // the network thread (and, in singleplayer, the local server, which can
    // take a while to save and exit) has actually wound down; resetting the
    // world any earlier would let still-arriving packets repopulate it. They
    // differ only in where the player lands afterward.
    Volcano::GUI::Screen* disconnectDestination = nullptr;
    auto beginDisconnect = [&state, &network, &disconnectDestination, loadingScreen](Volcano::GUI::Screen* destination) {
        disconnectDestination = destination;
        state.SetConnectionPhase(Volcano::ConnectionPhase::Disconnecting);
        if (network) network->RequestStop();
        Volcano::GUIController::OpenScreen(loadingScreen);
    };
    Volcano::GUIController::SetPauseScreen(Volcano::GUI::CreatePauseScreen(
        [beginDisconnect, connectScreen] { beginDisconnect(connectScreen); },
        [settingsScreen] { Volcano::GUI::OpenSettingsScreen(settingsScreen, Volcano::GUIController::GetPauseScreen()); },
        [beginDisconnect, mainMenuScreen] { beginDisconnect(mainMenuScreen); }));

    auto* singleplayerBackButton = new Volcano::GUI::Button("Back");
    singleplayerBackButton->AddClickHandler(new std::function<void()>(
        [mainMenuScreen] { Volcano::GUIController::OpenScreen(mainMenuScreen); }));
    singleplayerScreen->components.push_back(singleplayerBackButton);

    auto* connectBackButton = new Volcano::GUI::Button("Back");
    connectBackButton->AddClickHandler(new std::function<void()>(
        [mainMenuScreen] { Volcano::GUIController::OpenScreen(mainMenuScreen); }));
    connectScreen->components.push_back(connectBackButton);

    Volcano::GUIController::OpenScreen(mainMenuScreen);

    // This is now the frame loop itself (see RenderThread::RunFrame — it
    // calls glfwPollEvents() internally as the first step of PollInputs)
    // rather than a separate 1ms message-pump loop alongside a dedicated
    // render thread. Chunk/spawn handoff still happens entirely on
    // MeshingThread (see above).
    while (!state.shouldClose)
    {
        // GUIController and NotifyFramePresented run in this same loop now,
        // but GLFW still only guarantees glfwSetInputMode is safe from the
        // thread that created the window — this one, same as before, just
        // no longer a *different* thread than the one that just ran
        // PollInputs/DrawFrame — see GlobalState::pendingCursorMode.
        if (int mode = state.pendingCursorMode.exchange(-1); mode != -1)
        {
            glfwSetInputMode(state.window, GLFW_CURSOR, mode);
        }

        // NetworkThread flags this when a session ends unexpectedly (kicked,
        // dropped, login failure) — see GlobalState::ReportDisconnect. Reset
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

            // See GlobalState::ResetWorldState's own comment — same reset a
            // mid-session dimension change (PlayS2C::Respawn) uses, just
            // followed here by actually reopening the connect screen.
            state.SetConnectionPhase(Volcano::ConnectionPhase::Idle);
            state.ResetWorldState();

            disconnectReasonText->SetLabel(reason);
            disconnectReasonText->SetVisibile(!reason.empty());
            disconnectDestination = nullptr; // The unexpected disconnect wins over a pending player-requested one.
            Volcano::GUIController::OpenScreen(connectScreen);
        }

        // Finishes a player-requested leave (pause menu's Servers/Disconnect
        // — see beginDisconnect above) once nothing can repopulate the world.
        if (disconnectDestination != nullptr && (!network || network->HasSessionEnded()))
        {
            Volcano::GUI::Screen* destination = disconnectDestination;
            disconnectDestination = nullptr;

            state.SetConnectionPhase(Volcano::ConnectionPhase::Idle);
            state.ResetWorldState();

            disconnectReasonText->SetVisibile(false);
            Volcano::GUIController::OpenScreen(destination);
        }

        // The world actually streamed in (singleplayer or multiplayer alike)
        // — swap the loading screen out for the game itself. Checked after
        // the disconnect branch above (which already covers the failure
        // case: a connection dying before this ever becomes true reports
        // its own reason and reopens connectScreen instead, with the phase
        // back at Idle). Playing also keeps the menu panorama off across a
        // mid-session Respawn, where worldReady briefly drops again.
        if (state.connectionPhase.load() == Volcano::ConnectionPhase::LoadingTerrain && state.worldReady.load())
        {
            state.SetConnectionPhase(Volcano::ConnectionPhase::Playing);
            if (Volcano::GUIController::GetActiveScreen() == loadingScreen)
            {
                Volcano::GUIController::CloseScreen();
            }
        }

        renderer.RunFrame();
    }

    // Tear down the GUI and every render pass (waits for the GPU to finish
    // with them first) before renderEngine.Shutdown() below destroys the
    // device/window out from under them. No thread hand-off needed for this
    // anymore — RunFrame() above already stopped being called the moment
    // state.shouldClose went true, so nothing is still touching Vulkan/GLFW
    // by the time we get here. Previously nothing ever called
    // VulkanInit::Cleanup() at all: the process just exited with the Vulkan
    // device, swapchain, surface, instance and GLFW window all still alive,
    // which is what was hanging the graphics driver on shutdown (observed
    // on Windows/Intel Gen9) — the message-pump-while-stopping dance this
    // used to require was specifically to avoid that while a second thread
    // wound down; ThreadArchitecture.mdx's merge removes the second thread
    // instead of working around it.
    renderer.Shutdown();

    // MeshingThread must be fully joined before renderEngine.Shutdown()
    // below — it calls ChunkMesher::Shutdown(), which destroys the slab
    // buffers MeshingThread writes into.
    meshingThread.Stop();

    renderEngine.Shutdown();

    return 0;
}
