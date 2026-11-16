#pragma once
#ifndef VOLCANO_ENGINE_PASSES_TERRAIN_PASS_H
#define VOLCANO_ENGINE_PASSES_TERRAIN_PASS_H

#include "ChunkDrawBatch.hpp"
#include "ChunkMeshPass.hpp"

namespace Volcano::Engine::Passes {

// Opaque full-cube chunk geometry (GlobalState::renderList), frustum-culled
// per chunk
class TerrainPass : public ChunkMeshPass {
public:
    static constexpr const char* NAME = "Terrain";

    TerrainPass() : ChunkMeshPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;

private:
    // Graphics.IndirectDraw path
    ChunkDrawBatch batch;
    VisibleCache cache;
};

} // namespace Volcano::Engine::Passes

#endif
