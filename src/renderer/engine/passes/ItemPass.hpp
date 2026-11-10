#pragma once
#ifndef VOLCANO_ENGINE_PASSES_ITEM_PASS_H
#define VOLCANO_ENGINE_PASSES_ITEM_PASS_H

#include <chrono>
#include <cmath>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>
#include "../RenderPass.hpp"
#include "../RenderEngine.hpp"
#include "../../misc/ItemRenderer.hpp"
#include "../../terrain/OcclusionSnapshot.hpp"
#include "../../../GlobalState.hpp"
#include "../../../inventory/ItemRegistry.hpp"
#include "../../entity/models/EntityRegistry.hpp"

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
        GlobalState* state = frame.state;
        renderer = state->itemRenderer;
        if (renderer == nullptr) return;
        renderer->EnsureWorldPipeline(frame.engine->GetShaderLibrary());

        struct Drop
        {
            glm::vec3 position;
            int32_t itemId;
            uint32_t id;
        };
        std::vector<Drop> drops;
        struct Thrown
        {
            glm::vec3 position;
            const char* item;
            float height;
        };
        std::vector<Thrown> thrown;
        auto now = std::chrono::steady_clock::now();
        bool occlusionCulling = std::get<bool>(state->config->Get("Graphics.EntityOcclusionCulling", false));
        {
            std::lock_guard<std::mutex> lock(state->entitiesMutex);
            state->ForEachEntity([&](uint32_t id, const Entity& entity)
            {
                const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity.type);
                const char* thrownItem = info != nullptr ? EntityRegistry::ThrownItemName(info->name) : nullptr;
                if ((entity.itemId == 0 && thrownItem == nullptr) || !entity.visible) return;
                glm::vec3 position = entity.InterpolatedPosition(now);
                if (glm::distance(position, frame.scene->cameraPosition) > MAX_DISTANCE) return;
                if (occlusionCulling && occlusionSnapshot.IsEntityOccluded(position, entity.boundingBox.x, entity.boundingBox.y)) return;
                if (thrownItem != nullptr)
                {
                    thrown.push_back({ position, thrownItem, entity.boundingBox.y });
                    return;
                }
                drops.push_back({ position, entity.itemId, id });
            });
        }

        // Thrown items face the camera: the inverse of the view's rotation turns the sprite to look back down the ray.
        glm::mat4 facing(glm::transpose(glm::mat3(frame.scene->view)));
        for (const Thrown& item : thrown)
        {
            if (!renderer->HasModel(item.item)) continue;
            glm::mat4 placement = glm::translate(glm::mat4(1.0f), item.position + glm::vec3(0.0f, item.height * 0.5f, 0.0f)) * facing;
            renderer->RecordItem(frame.commandBuffer, item.item, "ground", placement, frame.viewProj);
        }

        for (const Drop& drop : drops)
        {
            const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(drop.itemId);
            if (item == nullptr || !renderer->HasModel(item->name)) continue;

            // Vanilla's bob (0.1 amplitude, ~3s period) and 1 rad/s spin, offset per entity.
            float phase = static_cast<float>(drop.id % 64) * 0.7f;
            float bob = std::sin(frame.scene->time * 2.0f + phase) * 0.1f + 0.1f;
            glm::mat4 placement = glm::translate(glm::mat4(1.0f), drop.position + glm::vec3(0.0f, 0.15f + bob, 0.0f));
            placement = glm::rotate(placement, frame.scene->time + phase, glm::vec3(0.0f, 1.0f, 0.0f));
            renderer->RecordItem(frame.commandBuffer, item->name, "ground", placement, frame.viewProj);
        }
    }

    void Destroy() override
    {
        if (renderer != nullptr) renderer->DestroyWorldPipeline();
        renderer = nullptr;
    }

private:
    ItemRenderer* renderer = nullptr;
};

} // namespace Volcano::Engine::Passes

#endif
