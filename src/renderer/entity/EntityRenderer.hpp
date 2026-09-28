#pragma once
#ifndef ENTITY_RENDERER_H
#define ENTITY_RENDERER_H

#include <cstdint>
#include <vulkan/vulkan.hpp>
#include <glm/glm.hpp>
#include "../../GlobalState.hpp"
#include "../engine/ShaderLibrary.hpp"

namespace Volcano {

// Renders GlobalState::entities as a second, batched draw pass alongside the
// chunk pass: one static cube mesh, one SSBO of per-instance data (position,
// yaw, bounding box, color/hurt tint) rebuilt every frame from the culled
// entity list, and a single vkCmdDrawIndexedIndirect() to draw them all —
// the CPU never issues one draw call per entity.
class EntityRenderer {
public:
    // Maximum entities drawn in a single frame — also the capacity of the
    // per-frame instance SSBO. Frustum culling plus this distance-sorted cap
    // are the "three culling methods" from the entity renderer plan, minus
    // depth testing (already free: entities share the chunk pass's
    // depth-tested pipeline/attachment).
    static constexpr uint32_t MAX_VISIBLE_ENTITIES = 256;

    // Creates the entity pipelines (entity.* and entity_textured.*,
    // resolved through shaders so a shader pack can replace them), static
    // meshes, skins, and per-frame-in-flight SSBO/indirect buffers. Must
    // run after VulkanInit::Init() (render pass, camera descriptor set
    // layout, command pool) — EntityPass::Create() calls it.
    static void Init(const Engine::ShaderLibrary& shaders);

    // Rebuilds just the two pipelines — for a shader reload. Meshes, skins
    // and buffers are left alone. The GPU must be idle.
    static void ReloadPipelines(const Engine::ShaderLibrary& shaders);

    // Destroys everything Init() created. Must run before the VMA allocator
    // is destroyed — EntityPass::Destroy() calls it from
    // RenderEngine::StopRendering().
    static void Shutdown();

    // Culls state->entities (frustum + distance limit), uploads the
    // survivors into frameIndex's instance SSBO, and issues one indirect
    // draw call for them. Must be called inside an active render pass with
    // viewport/scissor already set — binds its own pipeline and descriptor
    // sets but relies on the caller's dynamic state.
    static void RecordDraw(VkCommandBuffer cmd, uint32_t frameIndex, GlobalState* state,
        const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPosition);
};

} // namespace Volcano

#endif
