#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <stop_token>
#include <GLFW/glfw3.h>
#include "GlobalState.hpp"
#include "InputHandler.hpp"
#include "renderer/RenderThread.hpp"
#include "renderer/VulkanInit.hpp"
#include "renderer/TextureManager.hpp"
#include "renderer/terrain/models/Chunk.hpp"
#include "renderer/terrain/models/World.hpp"
#include "renderer/terrain/ChunkMesher.hpp"
#include "renderer/gui/GUIController.hpp"
#include "renderer/gui/models/Screen.hpp"
#include "renderer/gui/models/TextInput.hpp"
#include "renderer/gui/models/Button.hpp"
#include "network/NetworkThread.hpp"
#include "PlayerAttributes.hpp"
#include "TickLoop.hpp"
#include <vector>
using namespace std;

// Defaults for the connect screen's fields — this is the developer's own
// test server, prefilled purely for convenience; both fields are editable.
constexpr const char* DEV_SERVER_HOST = "108.197.182.119";
constexpr uint16_t DEV_SERVER_PORT = 25565;
constexpr const char* DEV_USERNAME = "VoidDev";

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
    std::cout << "[INFO] Generating 2x2 chunk grid..." << std::endl;

    // Generate 2x2 chunk grid (chunks 0 and 1 in both axes)
    for (int cx = 0; cx < 2; cx++) {
        for (int cz = 0; cz < 2; cz++) {
            // Built directly in World (not a local temporary) so TickLoop's
            // collision checks have real block data to query after this
            // function returns and the mesh-only view goes out of scope.
            Volcano::Chunk& chunk = state.world->GetOrCreateChunk(cx, cz);

            // Generate grass blocks for the top layer
            for (int x = 0; x < CHUNK_SIZE_X; x++) {
                for (int z = 0; z < CHUNK_SIZE_Z; z++) {
                    // Grass at y=1-3, dirt below, stone deeper
                    chunk.setBlock(x, 0, z, BlockType::Stone);
                    chunk.setBlock(x, 1, z, BlockType::Dirt);
                    chunk.setBlock(x, 2, z, BlockType::Dirt);
                    chunk.setBlock(x, 3, z, BlockType::Grass);
                }
            }

            // Add some variation - random trees
            if (cx % 2 == 0 && cz % 2 == 0) {
                int treeX = 7 + (cx * 7) % 9;
                int treeZ = 7 + (cz * 7) % 9;
                // Simple tree: log + leaves
                for (int y = 4; y <= 6; y++) {
                    chunk.setBlock(treeX, y, treeZ, BlockType::Wood);
                }
                // Leaves around the top
                for (int lx = -1; lx <= 1; lx++) {
                    for (int lz = -1; lz <= 1; lz++) {
                        if (lx != 0 || lz != 0) {
                            chunk.setBlock(treeX + lx, 6, treeZ + lz, BlockType::Leaves);
                        }
                    }
                }
                chunk.setBlock(treeX, 7, treeZ, BlockType::Leaves);
            }

            Volcano::Mesh chunkMesh = Volcano::ChunkMesher::MeshChunk(chunk, textureManager);

            // Position the chunk in world space
            chunkMesh.modelMatrix = glm::translate(glm::mat4(1.0f),
                glm::vec3(cx * CHUNK_SIZE_X, 0.0f, cz * CHUNK_SIZE_Z));

            state.renderList.push_back(chunkMesh);
        }
    }

    std::cout << "[INFO] Generated " << state.renderList.size() << " chunk meshes" << std::endl;
}

int main()
{
    // Let's keep this just for fun.
    cout << "[INFO] Hello, World!" << endl;

    // Initialize global state.
    Volcano::GlobalState state{};
    state.LoadSettings();

    // Create the render thread.
    Volcano::RenderThread renderer(&state);
    Init(&state);

    // Create the player.
    Volcano::Player player;
    // Position player in the center of the 4-chunk grid (chunks -1 to 1 in x/z)
    // Each chunk is 16 blocks, so center is at (8, 2, 8) offset from chunk origin
    player.SetPosition(glm::vec3(8.0f, 5.0f, 8.0f)); // Elevated to see the terrain
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
    // of the old fly-cam's continuous vertical axis.
    input.RegisterAction("Jump", InputActionTriggerType::PRESS, {GLFW_KEY_SPACE});

    // Initialize texture manager and load textures
    std::cout << "[INFO] Loading textures... (this may take a moment)" << std::endl;
    Volcano::TextureManager textureManager;
    textureManager.LoadResourcePack("resources/minecraft");
    std::cout << "[INFO] Texture loading complete!" << std::endl;

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
    Volcano::GUIController::Init(state.window, GetRenderPass(), 2, &state); // 2 is the swapchain image count

    // World (placeholder — see the tick-loop plan) and attributes must exist
    // before GenerateChunks/TickLoop touch them.
    Volcano::World world;
    state.world = &world;

    Volcano::PlayerAttributes attributes = Volcano::PlayerAttributes::Defaults();
    state.attributes = &attributes;

    // Generate chunks with grass blocks
    GenerateChunks(state, textureManager); // Generates 2x2 chunk grid (4 chunks total)

    // Constructed after player/input/world/attributes so its constructor
    // (which reads the player's starting position) has everything it needs.
    Volcano::TickLoop tickLoop(&state);
    state.tickLoop = &tickLoop;

    renderer.Start(); // Beyond this point, we need GLFW to be running.

    // Network runs on its own thread, deliberately separate from this
    // (GLFW-owning) main thread — see the networking plan for why. It's
    // only created once the player submits the connect screen below, since
    // the host/port/username aren't known until then.
    std::unique_ptr<Volcano::NetworkThread> network;

    // Connect screen: collects server address + username and starts the
    // network thread on "Connect". Shown immediately so the player never
    // sees a hardcoded server.
    Volcano::GUI::Screen* connectScreen = Volcano::GUIController::CreateScreen("Connect to Server", /* closable */ false);

    auto* addressInput = new Volcano::GUI::TextInput(
        "Server Address", std::string(DEV_SERVER_HOST) + ":" + std::to_string(DEV_SERVER_PORT));
    auto* usernameInput = new Volcano::GUI::TextInput("Username", DEV_USERNAME);
    auto* connectButton = new Volcano::GUI::Button("Connect");

    connectButton->AddClickHandler(new std::function<void()>([&state, &network, addressInput, usernameInput, connectScreen] {
        std::string host;
        uint16_t port;
        ParseServerAddress(addressInput->GetValue(), host, port, DEV_SERVER_PORT);
        std::string username = usernameInput->GetValue();
        if (host.empty() || username.empty()) return;

        network = std::make_unique<Volcano::NetworkThread>(&state, host, port, username);
        network->Start();

        Volcano::GUIController::CloseScreen();
    }));

    connectScreen->components.push_back(addressInput);
    connectScreen->components.push_back(usernameInput);
    connectScreen->components.push_back(connectButton);
    Volcano::GUIController::OpenScreen(connectScreen);

    // Handle inputs and stay alive.
    while (!state.shouldClose)
    {
        glfwPollEvents();
        this_thread::sleep_for(chrono::milliseconds(1));
    }

    // The render thread must fully stop touching Vulkan/GLFW before we tear
    // either down below — jthread's destructor would do this for us, but
    // only once `renderer` itself goes out of scope, which is after main()
    // returns. Previously nothing ever called VulkanInit::Cleanup() at all:
    // the process just exited with the Vulkan device, swapchain, surface,
    // instance and GLFW window all still alive, which is what was hanging
    // the graphics driver on shutdown (observed on Windows/Intel Gen9).
    renderer.Stop();
    Cleanup();

    return 0;
}
