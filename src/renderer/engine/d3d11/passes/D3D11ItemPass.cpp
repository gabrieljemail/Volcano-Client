#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "D3D11ItemPass.hpp"
#include "../D3D11ItemRenderer.hpp"
#include "../../../../ApplicationDeps.hpp"
import Volcano;

namespace Volcano::Engine::D3D11 {

void D3D11ItemPass::Record(FrameContext& frame)
{
    Application* state = frame.state;
    D3D11ItemRenderer* renderer = state->d3dItemRenderer;
    if (renderer == nullptr) return;

    EntityScene::CollectWorldItems(state, frame.scene->view, frame.scene->cameraPosition, frame.scene->time, items);
    for (const EntityScene::WorldItemDraw& item : items)
    {
        renderer->DrawItem(item.item, "ground", item.placement, frame.viewProj);
    }
}

} // namespace Volcano::Engine::D3D11
