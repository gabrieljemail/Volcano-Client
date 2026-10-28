#pragma once
#ifndef VOLCANO_ENGINE_PASSES_TERRAIN_PASS_H
#define VOLCANO_ENGINE_PASSES_TERRAIN_PASS_H

#include "ChunkDrawBatch.hpp"
#include "ChunkMeshPass.hpp"

namespace Volcano::Engine::Passes {

// Opaque full-cube chunk geometry (GlobalState::renderList), frustum-culled
// per chunk - the first pass of the frame, so it owns the terrain pipeline
// (and the pipelineLayout NonCubicPass reuses) plus its F3+Z wireframe twin.
// Shaders: terrain.vert / terrain.frag.
class TerrainPass : public ChunkMeshPass {
public:
    static constexpr const char* NAME = "Terrain";

    TerrainPass() : ChunkMeshPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;

private:
    // Graphics.IndirectDraw path - see ChunkDrawBatch. Only created when the
    // indirect pipelines exist.
    ChunkDrawBatch batch;
    VisibleCache cache;
};

} // namespace Volcano::Engine::Passes

#endif
