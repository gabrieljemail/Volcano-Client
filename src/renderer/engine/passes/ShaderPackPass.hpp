#pragma once
#ifndef VOLCANO_ENGINE_PASSES_SHADER_PACK_PASS_H
#define VOLCANO_ENGINE_PASSES_SHADER_PACK_PASS_H

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "../RenderPass.hpp"

namespace Volcano::Engine::Passes {

// The optional external-shader slot: a fullscreen pass that only exists
// when the active shader pack ships "overlay.vert.spv" + "overlay.frag.spv"
// (see ShaderLibrary). There's no built-in version — without a pack, or
// with a pack that doesn't provide both files, Create() builds nothing and
// Record() is a no-op, so the slot costs nothing when unused.
//
// Drawn after the world/selection passes and before the GUI, as one
// 3-vertex draw with no vertex buffer (the vertex shader derives a
// fullscreen triangle from gl_VertexIndex), alpha-blended over the frame
// with no depth test — tints, vignettes, fog-colored gradients and the
// like. It can't sample the frame itself: the whole frame is a single
// subpass, so there's no copy of the color target to read from yet.
//
// Shader interface (std430 push constants, both stages):
//   layout(push_constant) uniform Overlay {
//       vec4 viewport; // x: width px, y: height px, z: time s, w: delta time s
//       vec4 sky;      // rgb: sky/clear color, a: day brightness (0 midnight .. 1 noon)
//       vec4 camera;   // xyz: eye world position, w: 1 lighting on / 0 off
//   } overlay;
//   layout(set = 0, binding = 0) uniform CameraUBO { mat4 view; mat4 proj; } camera; // vertex stage only
class ShaderPackPass : public RenderPass {
public:
    static constexpr const char* NAME = "ShaderPackOverlay";
    static constexpr const char* VERTEX_SHADER = "overlay.vert";
    static constexpr const char* FRAGMENT_SHADER = "overlay.frag";

    struct PushConstants {
        glm::vec4 viewport;
        glm::vec4 sky;
        glm::vec4 camera;
    };

    ShaderPackPass() : RenderPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;

    // Whether the active pack supplied an overlay that built successfully.
    bool IsActive() const { return pipeline != VK_NULL_HANDLE; }

private:
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

} // namespace Volcano::Engine::Passes

#endif
