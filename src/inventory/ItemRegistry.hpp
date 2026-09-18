#pragma once
#ifndef VOLCANO_ITEM_REGISTRY_H
#define VOLCANO_ITEM_REGISTRY_H

#include <cstdint>
#include <string>

namespace Volcano {
class TextureManager;
}

namespace Volcano::ItemRegistry {

// One item type's static metadata, from minecraft-data's items.json plus a
// texture-array layer resolved against the already-loaded TextureManager.
struct ItemTypeInfo {
    std::string name;          // e.g. "diamond_sword" — matches the resource-pack texture stem for the common case.
    std::string displayName;   // e.g. "Diamond Sword".
    uint16_t textureLayer = 0; // TextureManager::array layer to sample for this item's icon.
    bool hasTexture = false;   // false if GetLayerIndex() found nothing under this name (see Init()'s own note).
};

// Parses minecraft-data's items.json once at startup, keyed by its "id"
// field — the same id sent in a Slot's itemId (see NetworkClient's Set
// Container Content/Slot handlers). Must run after TextureManager::
// LoadResourcePack(), since resolving each item's icon layer needs the
// texture array's name->layer table already built; not thread-safe with
// itself, but read-only (and thread-safe to call from any thread) after it
// returns — same contract as BlockRegistry::Init()/EntityRegistry::Init().
void Init(const TextureManager& textureManager);

// Returns the static metadata for a protocol item id, or nullptr if Init()
// hasn't run, the id is 0 ("air" — ItemStack's own "empty slot" convention,
// see ItemStack.hpp), or the id isn't in the loaded table.
const ItemTypeInfo* Lookup(int32_t protocolItemId);

} // namespace Volcano::ItemRegistry

#endif
