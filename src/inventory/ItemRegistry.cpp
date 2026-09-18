#include "ItemRegistry.hpp"
#include "Logger.hpp"
#include "renderer/TextureManager.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <unordered_map>

using json = nlohmann::json;

namespace Volcano::ItemRegistry {

namespace {

// Kept in sync with BlockRegistry/EntityRegistry's pinned minecraft-data
// version and PROTOCOL_VERSION (NetworkClient.hpp) — all must agree.
constexpr const char* MC_DATA_VERSION = "26.1";
const std::string ITEMS_JSON_PATH = std::string("resources/minecraft-data/data/pc/") + MC_DATA_VERSION + "/items.json";

std::unordered_map<int32_t, ItemTypeInfo> g_itemsById;

// Split out so the public Init() below can wrap it in a catch-all — a
// single malformed item entry must never be able to crash the whole app.
void InitImpl(const TextureManager& textureManager) {
    g_itemsById.clear();

    Log::Info("[INFO] ItemRegistry: reading " + ITEMS_JSON_PATH);

    std::ifstream file(ITEMS_JSON_PATH);
    if (!file) {
        Log::Error("[ERROR] ItemRegistry: couldn't open " + ITEMS_JSON_PATH
            + " — item lookups will find nothing this run.");
        return;
    }

    json itemsJson;
    try {
        file >> itemsJson;
    } catch (const std::exception& e) {
        Log::Error(std::string("[ERROR] ItemRegistry: failed to parse items.json: ") + e.what());
        return;
    }

    Log::Info("[INFO] ItemRegistry: parsed " + std::to_string(itemsJson.size()) + " item entries.");

    int resolvedIcons = 0;
    for (const json& i : itemsJson) {
        try {
            if (!i.contains("id") || !i["id"].is_number()) continue;
            int32_t id = i["id"].get<int32_t>();
            if (id == 0) continue; // "air" — ItemStack's own empty-slot id, see Lookup()'s comment; nothing to resolve.

            ItemTypeInfo info;
            info.name = i.value("name", std::string());
            info.displayName = i.value("displayName", std::string());

            // TextureManager's layerLookup is a single flat, bare-stem-keyed
            // table spanning both textures/block/ and textures/item/ (see
            // TextureManager::LoadResourcePack) — vanilla's own item/block
            // registry names don't collide between those two folders, so
            // looking the item's own name up directly (no item-model JSON
            // parsing to chase down which block texture a block-item
            // secretly reuses) resolves the common case correctly. A miss
            // just means this particular item has no exact-name 16x16
            // texture (an unusual icon shape, a name that doesn't match its
            // texture file, ...) — GetLayerIndex() already logs that case,
            // hasTexture just lets a caller skip drawing an icon instead of
            // drawing whatever GetLayerIndex()'s own fallback layer 0 is.
            info.hasTexture = textureManager.HasLayer(info.name);
            if (info.hasTexture) {
                info.textureLayer = textureManager.GetLayerIndex(info.name);
                resolvedIcons++;
            }

            g_itemsById.emplace(id, std::move(info));
        } catch (const std::exception& e2) {
            Log::Error(std::string("[ERROR] ItemRegistry: skipping a malformed item entry: ") + e2.what());
        }
    }

    Log::Info("[INFO] ItemRegistry: loaded minecraft-data " + std::string(MC_DATA_VERSION) + " — "
        + std::to_string(g_itemsById.size()) + " item types, " + std::to_string(resolvedIcons)
        + " with a resolved icon texture.");
}

} // namespace

void Init(const TextureManager& textureManager) {
    try {
        InitImpl(textureManager);
    } catch (const std::exception& e) {
        Log::Error(std::string("[ERROR] ItemRegistry: Init() failed unexpectedly, item lookups will find "
            "nothing this run: ") + e.what());
    }
}

const ItemTypeInfo* Lookup(int32_t protocolItemId) {
    auto it = g_itemsById.find(protocolItemId);
    return it != g_itemsById.end() ? &it->second : nullptr;
}

} // namespace Volcano::ItemRegistry
