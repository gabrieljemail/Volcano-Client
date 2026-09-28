#pragma once
#ifndef VOLCANO_ENGINE_FRAME_CONTEXT_H
#define VOLCANO_ENGINE_FRAME_CONTEXT_H

#include <cstdint>
#include <glm/glm.hpp>
#include "SceneView.hpp"

// Same opaque-handle typedef vulkan_core.h itself declares — repeating an
// identical typedef is legal, and it keeps this header (and therefore
// RenderPass.hpp and everything client-side that includes it) free of the
// full Vulkan headers.
typedef struct VkCommandBuffer_T* VkCommandBuffer;

namespace Volcano {
struct GlobalState;
}

namespace Volcano::Engine {

class RenderEngine;

// Output size in pixels — the swapchain extent on the Vulkan backend.
struct RenderExtent {
    uint32_t width = 0;
    uint32_t height = 0;
};

// Per-frame state handed to every RenderPass::Record() call, in pass order.
// Valid only for the duration of that call; nothing here should be cached
// across frames.
struct FrameContext {
    RenderEngine* engine = nullptr;
    GlobalState* state = nullptr;
    const SceneView* scene = nullptr;

    // Which per-frame-in-flight slot is being recorded (0..framesInFlight-1)
    // — the index for any per-frame buffers a pass keeps (camera UBO, entity
    // SSBOs, ...).
    uint32_t frameIndex = 0;
    RenderExtent extent;

    // Recording target. Already inside the main render pass with the
    // viewport/scissor set and the camera UBO for frameIndex uploaded, so a
    // pass only binds its own pipeline/descriptors and draws.
    VkCommandBuffer commandBuffer = nullptr;

    // proj * view, precomputed once per frame for frustum culling.
    glm::mat4 viewProj{1.0f};
};

} // namespace Volcano::Engine

#endif
