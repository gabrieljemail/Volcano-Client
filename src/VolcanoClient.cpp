#include <iostream>
#include <thread>
#include <chrono>
#include <stop_token>
#include <GLFW/glfw3.h>
#include "GlobalState.hpp"
#include "InputHandler.hpp"
#include "renderer/RenderThread.hpp"
#include "renderer/VulkanInit.hpp"
#include "renderer/terrain/models/Chunk.hpp"
#include "renderer/terrain/ChunkMesher.hpp"
using namespace std;

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

    // Normally, this is where the chunk mesh workers are set up.
    // Then the main menu is initialized after renderer.Start().
    // But we don't have either. So this is where the test chunk is.
    Chunk testChunk(0, 0);
    for (int x = 0; x < CHUNK_SIZE_X; x++)
        for (int z = 0; z < CHUNK_SIZE_Z; z++)
            for (int y = 0; y < 4; y++)
                testChunk.setBlock(x, y, z, BlockType::Stone);

    Mesh chunkMesh = ChunkMesher::MeshChunk(testChunk);
    state.renderList.push_back(chunkMesh);

    // Create the player.
    // My stoopid ahh didn't do this at first and caused a SIGSEGV,
    // don't be like me there!
    Volcano::Player player;
    state.player = &player;

    renderer.Start(); // Beyond this point, we need GLFW to be running.

    // Input handler.
    Volcano::InputHandler input(state.window);
    state.input = &input;

    // Register camera inputs.
    input.RegisterAxis("Camera.X", InputAxis{ -1.0f, 1.0f, 0.0f, -0.2f, 0, 0, AxisSourceType::MouseX, false });
    input.RegisterAxis("Camera.Y", InputAxis{ -1.0f, 1.0f, 0.0f, -0.2f, 0, 0, AxisSourceType::MouseY, false });

    // Handle inputs and stay alive.
    while (!state.shouldClose)
    {
        glfwPollEvents();
        this_thread::yield();
        // this_thread::sleep_for(chrono::milliseconds(1));
    }

    return 0;
}
