#pragma once
#ifndef VOLCANO_ENGINE_PASSES_SHADOW_PASS_H
#define VOLCANO_ENGINE_PASSES_SHADOW_PASS_H

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include "../RenderPass.hpp"
#include "../vulkan/VulkanRenderEngine.hpp"
#include "../../entity/EntityRenderer.hpp"
#include "ShadowSettings.hpp"

namespace Volcano::Engine::Passes {

// Optional sun shadow mapping (Graphics.Shadows.*, off by default).
//
// Recorded in the PRE_MAIN stage. The view distance out to Distance is split
// into three cascades (a tight near map, a middle one, a wide far one). Each
// frame every cascade's slice of the camera frustum is fitted with its own
// sphere, and the casters touching that light box are drawn depth-only into
// one layer of a DepthTarget array (made through VulkanRenderEngine's public
// API, so it is a template for mod passes). Receivers pick the tightest
// cascade covering a pixel, so close-up shadows get fine texels. The sun
// must be above the horizon; at night the map isn't drawn and IsActive() is
// false. Graphics.Shadows.DebugView tints shadowed terrain magenta and logs
// what each map drew.
//
// Recomputing: Graphics.Shadows.RecomputeOn picks how often the map is
// redrawn - every frame, per client tick, or per server tick. Between redraws
// the last map is reused (and still sampled, with its own matrix).
//
// Coverage:
//  2. Casters: terrain; non-cubic blocks (leaves, plants, glass, slabs), alpha-
//     tested so holes stay holes; textured mobs and players (animated, local
//     player included); and a hitbox for any other entity. Water doesn't cast.
//  3. Receivers: terrain, non-cubic blocks, water, textured entities, flat-
//     shaded entities and chests sample the map (PCF). Shadow darkens only the
//     sky-light term (the key light, on entities), so block light is never
//     shadowed. Entity casters are batched (instanced, indirect) and limited
//     to 96 blocks.
//  4. Block lights (SUN_AND_BLOCKS / EVERYTHING): not implemented. Only the
//     sun casts; the setting is read but those levels behave as SUN_ONLY.
class ShadowPass : public RenderPass {
public:
    static constexpr const char* NAME = "Shadows";
    static constexpr const char* VERTEX_SHADER = "shadow.vert";

    ShadowPass() : RenderPass(NAME) {}

    PassStage GetStage() const override { return PassStage::PRE_MAIN; }
    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;
    void ReloadShaders(const ShaderLibrary& shaders) override;

    // For receivers and mods. The map is valid to sample only while IsActive()
    // (it was drawn this frame); GetTarget() carries its array view (one layer
    // per cascade) and the compare sampler, GetCascades() the matrices.
    bool IsActive() const { return active; }
    const Vulkan::ShadowCascades& GetCascades() const { return cascades; }
    const Vulkan::DepthTarget& GetTarget() const { return target; }
    const ShadowSettings& GetSettings() const { return settings; }

private:
    ShadowSettings settings;
    Vulkan::VulkanRenderEngine* vulkan = nullptr;
    Vulkan::DepthTarget target;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    EntityRenderer::ShadowPipelines entityPipelines;
    VkPipelineLayout cutoutLayout = VK_NULL_HANDLE; // Texture array (set 0) + the light matrix.
    VkPipeline cutoutPipeline = VK_NULL_HANDLE;
    Vulkan::ShadowCascades cascades; // The last fitted cascades, resent on frames that reuse the map.
    float strength = 0.0f;        // How dark shadow makes sky light, 0-1.
    bool active = false;
    bool hasMap = false;          // The target holds a finished map from an earlier frame.
    uint64_t lastTickStamp = 0;   // The tick counter value the map was last drawn at.
    uint16_t builtDistance = 0;   // Distance the map was fitted with.
    uint32_t terrainCasters = 0;  // Sections drawn into the map last time (debug log).
    uint32_t cutoutCasters = 0;

    void ReleaseResources();
    bool EnsureTarget(uint32_t size);
    void CreatePipeline(const ShaderLibrary& shaders);
    void CreateCutoutPipeline(const ShaderLibrary& shaders);
    void DestroyPipeline();
    glm::mat4 FitSun(const SceneView& scene, glm::vec3 towardSun, float nearDistance, float farDistance, uint32_t mapSize,
        float& range, float& worldTexel) const;
    void RecordCascade(FrameContext& frame, uint32_t index);
    bool ShouldRecompute(const FrameContext& frame) const;
};

} // namespace Volcano::Engine::Passes

#endif
