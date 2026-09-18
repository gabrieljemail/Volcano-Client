#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <stop_token>
#include <GLFW/glfw3.h>
#include "GlobalState.hpp"
#include "InputHandler.hpp"
#include "Logger.hpp"
#include "renderer/RenderThread.hpp"
#include "renderer/VulkanInit.hpp"
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
#include "renderer/gui/models/TextInput.hpp"
#include "renderer/gui/models/Button.hpp"
#include "network/NetworkThread.hpp"
#include "PlayerAttributes.hpp"
#include "TickLoop.hpp"
#include "interaction/InteractionManager.hpp"
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
    Volcano::Log::Info("[INFO] Generating 2x2 chunk grid...");

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

            Volcano::Mesh chunkMesh = Volcano::ChunkMesher::MeshChunk(chunk, *state.world, textureManager);

            // Position the chunk in world space
            chunkMesh.modelMatrix = glm::translate(glm::mat4(1.0f),
                glm::vec3(cx * CHUNK_SIZE_X, 0.0f, cz * CHUNK_SIZE_Z));

            state.renderList.push_back(chunkMesh);
        }
    }

    Volcano::Log::Info("[INFO] Generated " + std::to_string(state.renderList.size()) + " chunk meshes");
}

int main()
{
    // Let's keep this just for fun.
    Volcano::Log::Info("[INFO] Hello, World!");

    // Initialize global state.
    Volcano::GlobalState state{};
    state.LoadSettings();

    // No Vulkan/GLFW dependency — safe this early, and the debug overlay
    // wants it available from the very first frame.
    state.systemInfo.DetectCPU();

    // Create the render thread.
    Volcano::RenderThread renderer(&state);
    Init(&state);

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
    // PrimaryAction is HOLD (vanilla lets holding the mouse down keep
    // attacking, rate-limited by its own cooldown).
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
    Volcano::Log::Info("[INFO] Loading textures... (this may take a moment)");
    Volcano::TextureManager textureManager;
    textureManager.LoadResourcePack("resources/minecraft");
    Volcano::Log::Info("[INFO] Texture loading complete!");

    // Resolves item icon texture layers against the array textureManager
    // just built — must run after LoadResourcePack(), not alongside
    // BlockRegistry/EntityRegistry::Init() above (which only need JSON,
    // not the texture array).
    Volcano::ItemRegistry::Init(textureManager);

    // Update texture descriptor set with the texture array
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = textureManager.array.view;
    imageInfo.sampler = textureManager.array.sampler;

    VkWriteDescriptorSet textureWrite{};
    textureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    textureWrite.dstSet = GetTextureSet();
    textureWrite.dstBinding = 0;
    textureWrite.descriptorCount = 1;
    textureWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureWrite.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(GetDevice(), 1, &textureWrite, 0, nullptr);

    // Initialize GUI controller framework with ImGui
    Volcano::GUIController::Init(state.window, GetRenderPass(), 2, &state, &textureManager); // 2 is the swapchain image count

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

    // Network runs on its own thread, deliberately separate from this
    // (GLFW-owning) main thread — see the networking plan for why. It's
    // only created once the player submits the connect screen below, since
    // the host/port/username aren't known until then.
    std::unique_ptr<Volcano::NetworkThread> network;

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

    connectButton->AddClickHandler(new std::function<void()>([&state, &network, addressInput, usernameInput, connectScreen, devServerPort] {
        std::string host;
        uint16_t port;
        ParseServerAddress(addressInput->GetValue(), host, port, devServerPort);
        std::string username = usernameInput->GetValue();
        if (host.empty() || username.empty()) return;

        network = std::make_unique<Volcano::NetworkThread>(&state, host, port, username);
        network->Start();

        Volcano::GUIController::CloseScreen();
    }));

    connectScreen->components.push_back(disconnectReasonText);
    connectScreen->components.push_back(addressInput);
    connectScreen->components.push_back(usernameInput);
    connectScreen->components.push_back(connectButton);
    Volcano::GUIController::OpenScreen(connectScreen);

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
            Volcano::Log::Info("[INFO] Returned to connect screen: " + reason);

            // See GlobalState::ResetWorldState's own comment — same reset a
            // mid-session dimension change (PlayS2C::Respawn) uses, just
            // followed here by actually reopening the connect screen.
            state.ResetWorldState();

            disconnectReasonText->SetLabel(reason);
            disconnectReasonText->SetVisibile(!reason.empty());
            Volcano::GUIController::OpenScreen(connectScreen);
        }

        renderer.RunFrame();
    }

    // Tear down the GUI/ImGui Vulkan resources (waits for the GPU to finish
    // with them first) before VulkanInit::Cleanup() below destroys the
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

    // MeshingThread must be fully joined before Cleanup() below — it calls
    // ChunkMesher::Shutdown(), which destroys the slab buffers MeshingThread
    // writes into.
    meshingThread.Stop();

    Cleanup();

    return 0;
}
