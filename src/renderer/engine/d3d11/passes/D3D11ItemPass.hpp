#pragma once
#ifndef VOLCANO_ENGINE_D3D11_ITEM_PASS_H
#define VOLCANO_ENGINE_D3D11_ITEM_PASS_H

#include <vector>
#include "../../RenderPass.hpp"
#include "../../../entity/EntityScene.hpp"

namespace Volcano::Engine::D3D11 {

// Direct3D 11 twin of the Vulkan ItemPass: dropped items (spinning and
// bobbing) and thrown ones (facing the camera) with a 3D model, placed by
// EntityScene::CollectWorldItems and drawn by D3D11ItemRenderer. Items without
// a model keep the entity pass's placeholder cube.
class D3D11ItemPass : public RenderPass {
public:
    static constexpr const char* NAME = "Items";

    D3D11ItemPass() : RenderPass(NAME) {}

    void Record(FrameContext& frame) override;

private:
    std::vector<EntityScene::WorldItemDraw> items;
};

} // namespace Volcano::Engine::D3D11

#endif
