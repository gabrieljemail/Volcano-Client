#pragma once
#ifndef VOLCANO_ENGINE_PASSES_ITEM_PASS_H
#define VOLCANO_ENGINE_PASSES_ITEM_PASS_H

#include <chrono>
#include <cmath>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>
#include "../../RenderPass.hpp"
#include "../../RenderEngine.hpp"
#include "../../../misc/ItemRenderer.hpp"
#include "../../../terrain/OcclusionSnapshot.hpp"
#include "../../../../ApplicationDeps.hpp"
#ifdef VOLCANO_MODULES
import Volcano;
#else
#include "../../../../Application.hpp"
#endif
#include "../../../../inventory/ItemRegistry.hpp"
#include "../../../entity/EntityScene.hpp"

namespace Volcano::Engine::Passes {

// Dropped items: entities with a stack (Entity::itemId) whose item has a 3D
// model, spun and bobbing like vanilla's. Items without a model are left to
// EntityRenderer's placeholder. Shaders: item.* (via ItemRenderer).
class ItemPass : public RenderPass {
public:
    static constexpr const char* NAME = "Items";
    static constexpr float MAX_DISTANCE = 64.0f;

    ItemPass() : RenderPass(NAME) {}

    void ReloadShaders(const ShaderLibrary&) override
    {
        if (renderer != nullptr) renderer->DestroyWorldPipeline();
    }

    void Record(FrameContext& frame) override
    {
        Application* state = frame.state;
        renderer = state->itemRenderer;
        if (renderer == nullptr) return;
        renderer->EnsureWorldPipeline(frame.engine->GetShaderLibrary());

        EntityScene::CollectWorldItems(state, frame.scene->view, frame.scene->cameraPosition, frame.scene->time, items);
        for (const EntityScene::WorldItemDraw& item : items)
        {
            if (!renderer->HasModel(item.item)) continue;
            renderer->RecordItem(frame.commandBuffer, item.item, "ground", item.placement, frame.viewProj);
        }
    }

    void Destroy() override
    {
        if (renderer != nullptr) renderer->DestroyWorldPipeline();
        renderer = nullptr;
    }

private:
    ItemRenderer* renderer = nullptr;
    std::vector<EntityScene::WorldItemDraw> items;
};

} // namespace Volcano::Engine::Passes

#endif
