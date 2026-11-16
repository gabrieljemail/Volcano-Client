#pragma once
#ifndef VOLCANO_ENGINE_PASSES_CHUNK_MESH_PASS_H
#define VOLCANO_ENGINE_PASSES_CHUNK_MESH_PASS_H

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <chrono>
#include "../RenderPass.hpp"
#include "../../VulkanInit.hpp"
#include <unordered_map>
#include <vector>
#include "../../models/Mesh.hpp"
#include "../../terrain/OcclusionSnapshot.hpp"
#include "../../terrain/VisibleChunkController.hpp"
#include "../../../GlobalState.hpp"

namespace Volcano::Engine::Passes {

// Shared base for the two passes that draw per-chunk Mesh lists (TerrainPass,
// NonCubicPass): both use VulkanInit's pipelineLayout (model-matrix push
// constant + camera set + texture array set), so binding the sets and
// issuing one mesh's draw are identical between them.
class ChunkMeshPass : public RenderPass {
public:
    using RenderPass::RenderPass;

protected:
    // One list's culled (and, for the blended lists, sorted) visible set from a
    // previous frame, reused while nothing it depends on has changed: the
    // camera (viewProj also carries its position, which the sort uses), the
    // mesh lists (GlobalState::meshListGeneration) and the Hi-Z snapshot
    // (OcclusionSnapshot::serial). Rarely hits while the camera moves, but
    // makes a standing-still frame skip the whole walk.
    struct VisibleCache {
        bool valid = false;
        glm::mat4 viewProj{1.0f};
        uint64_t generation = 0;
        uint64_t occlusionSerial = 0;
        uint32_t tested = 0, occluded = 0; // Hi-Z stats of the frame that filled it, re-added on a hit for the HUD.
        std::vector<const Mesh*> meshes;
    };

    // Culls `list` against the frustum and Hi-Z (back-to-front when
    // `sortFarthest`), or returns `cache` unchanged if it's still valid. The
    // caller holds the list's mutex, and the result is only good while it does.
    const std::vector<const Mesh*>& GetVisible(VisibleCache& cache,
        const std::unordered_map<SectionKey, Mesh, SectionKeyHash>& list, const FrameContext& frame, bool sortFarthest) const
    {
        GlobalState* state = frame.state;
        const uint64_t generation = state->meshListGeneration.load();
        if (cache.valid && cache.generation == generation && cache.occlusionSerial == occlusionSnapshot.serial
            && cache.viewProj == frame.viewProj)
        {
            if (occlusionSnapshot.collectStats)
            {
                occlusionSnapshot.tested += cache.tested;
                occlusionSnapshot.occluded += cache.occluded;
            }
            return cache.meshes;
        }

        const uint32_t testedBefore = occlusionSnapshot.tested, occludedBefore = occlusionSnapshot.occluded;
        {
            PhaseTimer timer(state, GlobalState::CHUNK_PHASE_CULL);
            cache.meshes = VisibleChunkController::GetVisibleMeshes(list, frame.viewProj);
        }
        cache.tested = occlusionSnapshot.tested - testedBefore;
        cache.occluded = occlusionSnapshot.occluded - occludedBefore;

        if (sortFarthest)
        {
            PhaseTimer timer(state, GlobalState::CHUNK_PHASE_SORT);
            const glm::vec3 cameraPosition = frame.scene->cameraPosition;
            std::sort(cache.meshes.begin(), cache.meshes.end(), [&cameraPosition](const Mesh* a, const Mesh* b)
            {
                glm::vec3 posA(a->modelMatrix[3]);
                glm::vec3 posB(b->modelMatrix[3]);
                float distA = glm::dot(posA - cameraPosition, posA - cameraPosition);
                float distB = glm::dot(posB - cameraPosition, posB - cameraPosition);
                return distA > distB; // Farthest first.
            });
        }

        cache.valid = true;
        cache.viewProj = frame.viewProj;
        cache.generation = generation;
        cache.occlusionSerial = occlusionSnapshot.serial;
        return cache.meshes;
    }

    // Times a phase of the chunk passes into the debug HUD's cull/sort/draw
    // line - a no-op (no clock reads) while the HUD is hidden.
    class PhaseTimer {
    public:
        PhaseTimer(GlobalState* stateIn, GlobalState::ChunkPhase phaseIn)
            : state(stateIn), phase(phaseIn), active(stateIn && stateIn->debugHudVisible)
        {
            if (active) start = std::chrono::steady_clock::now();
        }
        ~PhaseTimer()
        {
            if (active)
            {
                state->chunkPhaseFrameMs[phase] +=
                    std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
            }
        }
    private:
        GlobalState* state;
        GlobalState::ChunkPhase phase;
        bool active;
        std::chrono::steady_clock::time_point start;
    };

    // Camera UBO for this frame slot (set 0) + the block texture array (set 1).
    void BindWorldDescriptorSets(const FrameContext& frame) const
    {
        VkDescriptorSet sets[] = { cameraSets[frame.frameIndex], textureSet };
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 2, sets, 0, nullptr);
    }

    // `category` is which GlobalState::trianglesDrawn bucket this mesh's
    // triangles count toward (debug HUD only, skipped while it's hidden).
    void DrawMesh(VkCommandBuffer cmd, const Mesh& mesh, GlobalState* state, GlobalState::TriangleCategory category) const
    {
        VkDeviceSize offsets[] = {mesh.vertexOffset};
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertexBuffer, offsets);
        struct PushConstants
        {
            glm::mat4 model;
            float fade;
        } push{mesh.modelMatrix, 1.0f};
        if (mesh.fadeStart.time_since_epoch().count() != 0)
        {
            constexpr float FADE_SECONDS = 0.75f;
            float elapsed = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - mesh.fadeStart).count();
            push.fade = std::clamp(elapsed / FADE_SECONDS, 0.0f, 1.0f);
        }
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

        if (mesh.indexCount > 0)
        {
            vkCmdBindIndexBuffer(cmd, mesh.indexBuffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
            if (state && state->debugHudVisible) state->trianglesDrawn[category] += mesh.indexCount / 3;
        } else
        {
            vkCmdDraw(cmd, mesh.vertexCount, 1, 0, 0);
            if (state && state->debugHudVisible) state->trianglesDrawn[category] += mesh.vertexCount / 3;
        }
    }
};

} // namespace Volcano::Engine::Passes

#endif
