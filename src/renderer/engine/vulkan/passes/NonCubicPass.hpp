#pragma once
#ifndef VOLCANO_ENGINE_PASSES_NON_CUBIC_PASS_H
#define VOLCANO_ENGINE_PASSES_NON_CUBIC_PASS_H

#include "ChunkDrawBatch.hpp"
#include "ChunkMeshPass.hpp"

namespace Volcano::Engine::Passes {

// Transparent full cubes (glass, slime, ice, ...), partial-volume shapes
// (slabs, carpets, ...) and cross-shaped plants (grass, flowers, ...) from
// GlobalState::nonCubicRenderList
class NonCubicPass : public ChunkMeshPass {
public:
    static constexpr const char* NAME = "NonCubic";

    NonCubicPass() : ChunkMeshPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;

private:
    // Graphics.IndirectDraw path (one per list)
    ChunkDrawBatch cutoutBatch;
    ChunkDrawBatch translucentBatch;
    VisibleCache cutoutCache;
    VisibleCache translucentCache;
};

} // namespace Volcano::Engine::Passes

#endif
