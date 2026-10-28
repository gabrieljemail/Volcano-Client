#pragma once
#ifndef VOLCANO_BLOCKENTITY_MODELED_BE_RENDERER_H
#define VOLCANO_BLOCKENTITY_MODELED_BE_RENDERER_H

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <glm/glm.hpp>
#include "../entity/EntityRenderer.hpp"

namespace Volcano {

class World;

// Finds block entities that draw as a model (the chest family so far) near
// the camera and hands the render thread a ready-to-draw list. A background
// thread rescans the world every half second, so placed or broken ones show
// up within that window. New kinds (signs, beds, banners, shulker boxes...)
// plug in through EntityRenderer's block-entity model table.
class ModeledBERenderer {
public:
    ~ModeledBERenderer();

    // Starts the scan thread against `world`. Safe to call again after Stop().
    void Start(World* const* worldPointer);
    void Stop();

    // Render thread: tells the scanner where the camera is.
    void SetCameraPosition(const glm::vec3& position);

    // Render thread: copies the latest scan result.
    void CopyDraws(std::vector<EntityRenderer::ModeledBEDraw>& out);

private:
    void Run();

    World* const* worldPointer = nullptr;
    std::thread thread;
    std::atomic<bool> running{ false };
    std::atomic<float> cameraX{ 0.0f };
    std::atomic<float> cameraY{ 0.0f };
    std::atomic<float> cameraZ{ 0.0f };
    std::mutex mutex;
    std::vector<EntityRenderer::ModeledBEDraw> draws;
};

} // namespace Volcano

#endif
