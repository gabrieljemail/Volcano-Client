#pragma once
#ifndef VOLCANO_ENGINE_PASSES_NON_CUBIC_PASS_H
#define VOLCANO_ENGINE_PASSES_NON_CUBIC_PASS_H

#include "ChunkMeshPass.hpp"

namespace Volcano::Engine::Passes {

// Transparent full cubes (glass, slime, ice, ...), partial-volume shapes
// (slabs, carpets, ...) and cross-shaped plants (grass, flowers, ...) from
// GlobalState::nonCubicRenderList — see NonCubicMesher and VulkanInit's
// nonCubicPipeline. Alpha-blended with depth writes ON (see that
// pipeline's own comment for why this is really a cutout pass), drawn
// after opaque terrain and entities, chunks sorted back-to-front.
// Must come after TerrainPass: it reuses that pass's pipelineLayout.
// Shaders: misc.vert / misc.frag.
class NonCubicPass : public ChunkMeshPass {
public:
    static constexpr const char* NAME = "NonCubic";

    NonCubicPass() : ChunkMeshPass(NAME) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;
};

} // namespace Volcano::Engine::Passes

#endif
