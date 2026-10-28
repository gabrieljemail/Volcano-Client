#include "ModeledBERenderer.hpp"

#include <chrono>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include "../terrain/models/World.hpp"

namespace Volcano {

namespace {

constexpr int SCAN_RADIUS = 48;
constexpr int SCAN_HEIGHT = 32;

// Render yaw that points the chest's front (the lock side) along `facing`
// (0 north, 1 south, 2 west, 3 east). The mesh's front sits on local -Z.
float YawForFacing(int facing)
{
    switch (facing)
    {
    case 1: return glm::pi<float>();
    case 2: return glm::half_pi<float>();
    case 3: return -glm::half_pi<float>();
    default: return 0.0f;
    }
}

} // namespace

ModeledBERenderer::~ModeledBERenderer()
{
    Stop();
}

void ModeledBERenderer::Start(World* const* worldPtr)
{
    Stop();
    worldPointer = worldPtr;
    running = true;
    thread = std::thread(&ModeledBERenderer::Run, this);
}

void ModeledBERenderer::Stop()
{
    running = false;
    if (thread.joinable()) thread.join();
    std::lock_guard lock(mutex);
    draws.clear();
}

void ModeledBERenderer::SetCameraPosition(const glm::vec3& position)
{
    cameraX = position.x;
    cameraY = position.y;
    cameraZ = position.z;
}

void ModeledBERenderer::CopyDraws(std::vector<EntityRenderer::ModeledBEDraw>& out)
{
    std::lock_guard lock(mutex);
    out = draws;
}

void ModeledBERenderer::Run()
{
    while (running)
    {
        World* world = worldPointer != nullptr ? *worldPointer : nullptr;
        if (world != nullptr)
        {
            glm::ivec3 center(static_cast<int>(std::floor(cameraX.load())),
                static_cast<int>(std::floor(cameraY.load())), static_cast<int>(std::floor(cameraZ.load())));
            glm::ivec3 extent(SCAN_RADIUS, SCAN_HEIGHT, SCAN_RADIUS);

            std::vector<EntityRenderer::ModeledBEDraw> found;
            world->ForEachBlockEntityInBox(center - extent, center + extent,
                [&found](glm::ivec3 position, const Block& block)
                {
                    EntityRenderer::ModeledBEDraw draw;
                    draw.position = glm::vec3(position.x + 0.5f, static_cast<float>(position.y), position.z + 0.5f);
                    draw.yaw = YawForFacing(block.blockEntityData & 3);
                    draw.kind = block.blockEntityKind;
                    draw.type = (block.blockEntityData >> 2) & 3;
                    found.push_back(draw);
                });

            std::lock_guard lock(mutex);
            draws = std::move(found);
        }

        for (int i = 0; i < 5 && running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace Volcano
