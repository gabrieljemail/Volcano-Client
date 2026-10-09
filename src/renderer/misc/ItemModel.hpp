#pragma once
#ifndef VOLCANO_ITEM_MODEL_H
#define VOLCANO_ITEM_MODEL_H

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "ModelLoader.hpp"

namespace Volcano {

class TextureManager;

enum class ItemModelType : uint8_t {
    ITEM,       // Extruded item texture, about 0.05m big.
    BLOCK,      // A textured cube about 0.15m big.
    CUSTOM      // A custom model in the RP, scaled to about 0.15m big.
};

// Vertex of an item mesh: model space is 0..1 per axis (like the resource
// pack's 0..16 / 16), `layer` indexes TextureManager's texture array.
struct ItemModelVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv; // 0..1 within the 16x16 layer.
    float layer;
};

// The 3D shape of one item, built from the resource pack: blocks and custom
// models from their JSON elements, flat items extruded from their sprite.
// Shared by every renderer that draws items (inventory icons, the hand,
// dropped items). Meshes are built once per item and cached.
class ItemModel {
public:
    // nullptr when the item has no drawable model (e.g. entity-rendered
    // items like chests and beds, which have no JSON elements).
    static const ItemModel* ForItem(const std::string& itemName, const TextureManager& textures);

    ItemModelType GetType() const { return type; }
    const std::vector<ItemModelVertex>& GetVertices() const { return vertices; }
    const std::vector<uint32_t>& GetIndices() const { return indices; }

    // Flat items are lit head-on (vanilla's "front" gui light) instead of
    // from above-left like blocks.
    bool IsFlat() const { return type == ItemModelType::ITEM; }

    // The model's display transform for `context` ("gui", "ground",
    // "firstperson_righthand", "thirdperson_righthand", "fixed", ...) as a
    // matrix: translate * rotate(X, Y, Z) * scale, then recentred so the
    // model's middle sits at the origin — same order as vanilla's
    // ItemTransform.apply. Identity-ish if the model has no entry for it.
    glm::mat4 DisplayMatrix(const std::string& context) const;

private:
    ItemModelType type = ItemModelType::ITEM;
    std::vector<ItemModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::unordered_map<std::string, ModelLoader::DisplayTransform> display;

    friend class ItemModelBuilder;
};

} // namespace Volcano

#endif
