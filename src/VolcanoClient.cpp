#include <iostream>
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
#include "renderer/terrain/ChunkMesher.hpp"
#include <vector>
using namespace std;

// Chunk generation function for 4-chunk view distance (2x2 grid)
void GenerateChunks(Volcano::GlobalState& state, const Volcano::TextureManager& textureManager) {
    std::cout << "[INFO] Generating 2x2 chunk grid..." << std::endl;

    // Generate 2x2 chunk grid (chunks 0 and 1 in both axes)
    for (int cx = 0; cx < 2; cx++) {
        for (int cz = 0; cz < 2; cz++) {
            Volcano::Chunk chunk(cx, cz);

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

    // Register camera inputs.
    input.RegisterAxis("Camera.X", InputAxis{ -1.0f, 1.0f, 0.0f, 0.01f, 0, 0, AxisSourceType::MouseX, false });
    input.RegisterAxis("Camera.Y", InputAxis{ -1.0f, 1.0f, 0.0f, 0.01f, 0, 0, AxisSourceType::MouseY, false });

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

    // Generate chunks with grass blocks
    GenerateChunks(state, textureManager); // Generates 2x2 chunk grid (4 chunks total)

    renderer.Start(); // Beyond this point, we need GLFW to be running.

    // Handle inputs and stay alive.
    while (!state.shouldClose)
    {
        glfwPollEvents();
        this_thread::sleep_for(chrono::milliseconds(1));
    }

    return 0;
}
