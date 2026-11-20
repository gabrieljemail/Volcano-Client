#pragma once
#ifndef VOLCANO_SELECTION_SCENE_H
#define VOLCANO_SELECTION_SCENE_H

#include <vector>
#include <glm/glm.hpp>
#include "../../ApplicationDeps.hpp"
#ifdef VOLCANO_MODULES
import Volcano;
#else
#include "../../Application.hpp"
#endif

namespace Volcano::SelectionScene {

// Nudges every box just outside the block's real faces so the coplanar overlay
// doesn't z-fight against the block itself (the depth bias does the rest).
constexpr float INFLATE = 0.002f;

// One box of a block's collision shape (a stair has several), in world space.
struct Box {
    glm::vec3 min;
    glm::vec3 max;
};

// The break-crack overlay on one block: the destroy_stage texture to use
// (0-9) and the boxes to draw it over.
struct Crack {
    glm::ivec3 position;
    int stage = 0;
    std::vector<Box> boxes;
};

// What one frame draws for the targeted block, shared by the Vulkan
// SelectionRenderer and the Direct3D 11 selection pass.
struct Frame {
    std::vector<Crack> cracks; // Our own mining first, then everyone else's; draw before the outline.
    std::vector<Box> boxes;    // The targeted block's shape; empty when nothing is targeted.
    glm::vec4 outlineColor{0.0f};
    glm::vec4 fillColor{0.0f};
};

// Fills `out` (which it clears) from the interaction manager's targeted block,
// our mining progress and the other players' block breaks. Render thread.
void Collect(Application* state, Frame& out);

} // namespace Volcano::SelectionScene

#endif
