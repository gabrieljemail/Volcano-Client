#pragma once
#ifndef ENTITY_RENDERER_H
#define ENTITY_RENDERER_H

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "../../GlobalState.hpp"
#include "../engine/ShaderLibrary.hpp"

namespace Volcano {

namespace Engine::Vulkan { class VulkanRenderEngine; }

// Renders GlobalState::entities as a second, batched draw pass alongside the
// chunk pass: one static cube mesh, one SSBO of per-instance data (position,
// yaw, bounding box, color/hurt tint) rebuilt every frame from the culled
// entity list, and a single vkCmdDrawIndexedIndirect() to draw them all -
// the CPU never issues one draw call per entity.
class EntityRenderer {
public:
    // Maximum entities drawn in a single frame - also the capacity of the
    // per-frame instance SSBO. Frustum culling plus this distance-sorted cap
    // are the "three culling methods" from the entity renderer plan, minus
    // depth testing (already free: entities share the chunk pass's
    // depth-tested pipeline/attachment).
    static constexpr uint32_t MAX_VISIBLE_ENTITIES = 256;

    // Creates the entity pipelines (entity.* and entity_textured.*,
    // resolved through shaders so a shader pack can replace them), static
    // meshes, skins, and per-frame-in-flight SSBO/indirect buffers. Must
    // run after VulkanInit::Init() (render pass, camera descriptor set
    // layout, command pool) - EntityPass::Create() calls it.
    static void Init(const Engine::ShaderLibrary& shaders);

    // Rebuilds just the two pipelines - for a shader reload. Meshes, skins
    // and buffers are left alone. The GPU must be idle.
    static void ReloadPipelines(const Engine::ShaderLibrary& shaders);

    // Rebuilds every pack-sourced texture (mob/chest skins, block-entity
    // textures) after a resource reload. GPU must be idle.
    static void ReloadTextures();

    // Destroys everything Init() created. Must run before the VMA allocator
    // is destroyed - EntityPass::Destroy() calls it from
    // RenderEngine::StopRendering().
    static void Shutdown();

    // The default (Steve) skin as a sampler2D descriptor set, and its layout,
    // for the first-person arm. Valid between Init() and Shutdown().
    static VkDescriptorSet GetDefaultSkinSet();
    static VkDescriptorSetLayout GetSkinSetLayout();

    // One modeled block entity to draw: position is the block centre at floor
    // height, yaw is the render yaw (radians), kind is Block::blockEntityKind
    // (1 chest / 2 trapped / 3 ender) and type the model variant
    // (chest: 0 single / 1 left / 2 right).
    struct ModeledBEDraw
    {
        glm::vec3 position;
        float yaw;
        uint8_t kind;
        uint8_t type;
    };

    // Draws block-entity models with the textured entity pipeline: frustum
    // culled, grouped by mesh and skin, one instanced draw per group (an
    // indirect one when `indirect` and the device supports it). Same render-pass
    // requirements as RecordDraw. Returns the triangles drawn.
    // `vulkan` (optional) lets them receive the shadow map in frames that have one.
    static uint64_t RecordBlockEntities(VkCommandBuffer cmd, uint32_t frameIndex, const std::vector<ModeledBEDraw>& draws,
        const glm::mat4& viewProj, bool indirect, Engine::Vulkan::VulkanRenderEngine* vulkan = nullptr);

    // Depth-only entity pipelines for a shadow map's render pass (see
    // ShadowPass): one per vertex format - textured mob meshes, and the plain
    // box used for everything without a model.
    struct ShadowPipelines
    {
        VkPipeline textured = VK_NULL_HANDLE;
        VkPipeline box = VK_NULL_HANDLE;
    };

    // `layout` must have a single vertex-stage mat4 push constant (the light
    // matrix); every part brings its own model matrix as instance data. Throws
    // if shadow_entity.vert is missing.
    static void CreateShadowPipelines(const Engine::ShaderLibrary& shaders, VkRenderPass shadowRenderPass,
        VkPipelineLayout layout, ShadowPipelines& out);
    static void DestroyShadowPipelines(ShadowPipelines& pipelines);

    // Records every entity that can shadow the light's box into the shadow
    // map's open render pass: animated textured mobs and players (the local
    // player included, in any perspective), and a bounding box for the rest.
    // `distance` limits casters to that many blocks around the camera (and
    // never beyond a fixed cap). Parts are batched: identical parts are one
    // instanced command and each mesh one draw, indirect when `indirect` is set
    // and supported. `cascade` (0-2) picks which of this frame's caster buffers
    // to use, so each cascade needs its own index.
    static void RecordShadowCasters(VkCommandBuffer cmd, VkPipelineLayout layout, const ShadowPipelines& pipelines,
        GlobalState* state, const glm::mat4& lightViewProj, const glm::vec3& cameraPosition, float distance,
        uint32_t frameIndex, uint32_t cascade, bool indirect);

    // Culls state->entities (frustum + distance limit), uploads the
    // survivors into frameIndex's instance SSBO, and issues one indirect
    // draw call per shape for the flat-shaded ones. Textured mobs, players and
    // block entities are grouped by mesh and skin and drawn instanced, through
    // indirect draws when `indirect` is set and the device supports it. Must be
    // called inside an active render pass with viewport/scissor already set - binds its own pipeline and descriptor
    // sets but relies on the caller's dynamic state.
    static void RecordDraw(VkCommandBuffer cmd, uint32_t frameIndex, GlobalState* state,
        const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPosition, bool indirect = true);
};

} // namespace Volcano

#endif
