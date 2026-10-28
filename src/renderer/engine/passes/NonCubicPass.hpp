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
//
// Also owns the genuinely translucent pass (currently water only, from
// GlobalState::translucentRenderList — see VulkanInit's translucentPipeline)
// drawn right after the cutout pass above with depth WRITE off, so it's
// grouped here rather than as its own RenderPass since it shares every
// other detail (shaders, vertex format, pipelineLayout) with the cutout pass.
//
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
