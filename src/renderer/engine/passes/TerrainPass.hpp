#pragma once
#ifndef VOLCANO_ENGINE_PASSES_TERRAIN_PASS_H
#define VOLCANO_ENGINE_PASSES_TERRAIN_PASS_H

#include "ChunkMeshPass.hpp"

namespace Volcano::Engine::Passes {

// Opaque full-cube chunk geometry (GlobalState::renderList), frustum-culled
// per chunk — the first pass of the frame, so it owns the terrain pipeline
// (and the pipelineLayout NonCubicPass reuses) plus its F3 wireframe twin.
// Shaders: terrain.vert / terrain.frag.
class TerrainPass : public ChunkMeshPass {
public:
    static constexpr const char* NAME = "Terrain";

    TerrainPass() : ChunkMeshPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;
};

} // namespace Volcano::Engine::Passes

#endif
