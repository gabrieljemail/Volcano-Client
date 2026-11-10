#pragma once
#ifndef VOLCANO_ENGINE_PASSES_SHADER_PACK_PASS_H
#define VOLCANO_ENGINE_PASSES_SHADER_PACK_PASS_H

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "../RenderPass.hpp"

namespace Volcano::Engine::Passes {

// The optional external-shader slot: a fullscreen pass that only exists
// when the active shader pack ships "overlay.vert.spv" + "overlay.frag.spv"
// (see ShaderLibrary). There's no built-in version
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
