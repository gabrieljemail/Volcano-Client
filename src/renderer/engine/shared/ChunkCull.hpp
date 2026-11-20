#pragma once
#ifndef VOLCANO_ENGINE_PASSES_CHUNK_CULL_H
#define VOLCANO_ENGINE_PASSES_CHUNK_CULL_H

// Backend-neutral half of the chunk-mesh passes: everything that decides
// WHICH meshes get drawn, with no graphics-API calls at all. The Vulkan
// ChunkMeshPass and a future Direct3D 11 equivalent both derive from this so
// frustum culling, Hi-Z occlusion testing, back-to-front sorting, the
// per-frame visible-set cache and the chunk fade are written once.
//
// What is NOT here (each backend provides its own):
//   - pipeline creation and binding        -> Create()/Record() of the pass
//   - camera UBO / constant buffer binding -> the pass's per-frame setup
//   - texture array binding                -> the pass's per-frame setup
//   - the actual vertex/index draws        -> the pass's DrawMesh equivalent
//   - Mesh::vertexBuffer/indexBuffer casts -> backend buffer handle type

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "../FrameContext.hpp"
#include "../../models/Mesh.hpp"
#include "../../terrain/OcclusionSnapshot.hpp"
#include "../../terrain/VisibleChunkController.hpp"
#include "../../../ApplicationDeps.hpp"
#ifdef VOLCANO_MODULES
import Volcano;
#else
#include "../../../Application.hpp"
#endif

namespace Volcano::Engine::Passes {

class ChunkCull {
protected:
    // One list's culled (and, for the blended lists, sorted) visible set from a
    // previous frame, reused while nothing it depends on has changed: the
    // camera (viewProj also carries its position, which the sort uses), the
    // mesh lists (Application::meshListGeneration) and the Hi-Z snapshot
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
        Application* state = frame.state;
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
            PhaseTimer timer(state, Application::CHUNK_PHASE_CULL);
            cache.meshes = VisibleChunkController::GetVisibleMeshes(list, frame.viewProj);
        }
        cache.tested = occlusionSnapshot.tested - testedBefore;
        cache.occluded = occlusionSnapshot.occluded - occludedBefore;

        if (sortFarthest)
        {
            PhaseTimer timer(state, Application::CHUNK_PHASE_SORT);
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
        PhaseTimer(Application* stateIn, Application::ChunkPhase phaseIn)
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
        Application* state;
        Application::ChunkPhase phase;
        bool active;
        std::chrono::steady_clock::time_point start;
    };

    // How long a freshly-meshed chunk takes to fade in (seconds). Shared by
    // every backend's per-mesh draw path; keep in sync with ChunkDrawBatch's
    // indirect path, which inlines the same constant.
    static constexpr float FADE_SECONDS = 0.75f;

    // 1.0 for a mesh that isn't fading; otherwise 0 -> 1 over FADE_SECONDS
    // since fadeStart. Pass the frame's `now` if drawing many meshes so they
    // all sample the same instant.
    static float FadeAmount(const Mesh& mesh, std::chrono::steady_clock::time_point now)
    {
        if (mesh.fadeStart.time_since_epoch().count() == 0) return 1.0f;
        float elapsed = std::chrono::duration<float>(now - mesh.fadeStart).count();
        return std::clamp(elapsed / FADE_SECONDS, 0.0f, 1.0f);
    }
};

} // namespace Volcano::Engine::Passes

#endif
