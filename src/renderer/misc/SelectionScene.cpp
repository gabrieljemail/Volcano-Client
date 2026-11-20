#include "SelectionScene.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include "../terrain/models/BlockRegistry.hpp"
#include "../terrain/models/World.hpp"
#include "../../interaction/InteractionManager.hpp"

namespace Volcano::SelectionScene {

namespace {

// The block's collision shape as inflated world-space boxes (empty for a block with none).
std::vector<Box> ShapeBoxes(Application* state, const glm::ivec3& position)
{
    std::vector<Box> boxes;
    Block block = state->world->GetBlock(position.x, position.y, position.z);
    BlockRegistry::CollisionBoxes shape = BlockRegistry::GetCollisionBoxes(block);
    glm::vec3 origin(position);
    for (int i = 0; i < shape.count; i++)
    {
        const BlockRegistry::AABB& box = shape.boxes[static_cast<size_t>(i)];
        boxes.push_back({ origin + box.min - glm::vec3(INFLATE), origin + box.max + glm::vec3(INFLATE) });
    }
    return boxes;
}

} // namespace

void Collect(Application* state, Frame& out)
{
    out.cracks.clear();
    out.boxes.clear();

    std::optional<TargetedBlock> target = state->interaction->GetTargetedBlock();

    auto addCrack = [&](const glm::ivec3& position, int stage)
    {
        Crack crack;
        crack.position = position;
        crack.stage = stage;
        crack.boxes = ShapeBoxes(state, position);
        if (!crack.boxes.empty()) out.cracks.push_back(std::move(crack));
    };

    // Cracks first, so the outline draws over them: our own mining, then everyone else's.
    int32_t ticksRequired = state->miningTicksRequired.load(std::memory_order_relaxed);
    if (target && ticksRequired > 0)
    {
        int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        float ticks = static_cast<float>(nowNs - state->miningStartNs.load(std::memory_order_relaxed)) * 1e-9f * 20.0f;
        int stage = std::clamp(static_cast<int>(ticks / static_cast<float>(ticksRequired) * 10.0f), 0, 9);
        addCrack(target->position, stage);
    }
    {
        std::lock_guard<std::mutex> lock(state->blockBreaksMutex);
        for (const auto& [breaker, progress] : state->blockBreaks) addCrack(progress.position, progress.stage);
    }

    if (!target) return;
    out.boxes = ShapeBoxes(state, target->position); // Empty for a shape-less block (shouldn't happen: only solid blocks get targeted).

    Color4i outline = state->interaction->GetSelectionOutlineColor();
    Color4i fill = state->interaction->GetSelectionFillColor();
    out.outlineColor = glm::vec4(outline.r / 255.0f, outline.g / 255.0f, outline.b / 255.0f, outline.a);
    out.fillColor = glm::vec4(fill.r / 255.0f, fill.g / 255.0f, fill.b / 255.0f, fill.a);
}

} // namespace Volcano::SelectionScene
