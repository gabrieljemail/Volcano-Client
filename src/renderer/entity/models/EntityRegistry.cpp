#include "EntityRegistry.hpp"
#include "Logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <unordered_map>

using json = nlohmann::json;

namespace Volcano::EntityRegistry {

namespace {

// Kept in sync with BlockRegistry's pinned minecraft-data version and
// PROTOCOL_VERSION (NetworkClient.hpp) — all three must agree.
constexpr const char* MC_DATA_VERSION = "26.1";
const std::string ENTITIES_JSON_PATH = std::string("resources/minecraft-data/data/pc/") + MC_DATA_VERSION + "/entities.json";

std::unordered_map<uint32_t, EntityTypeInfo> g_entitiesById;

// Split out so the public Init() below can wrap it in a catch-all — a single
// malformed entity entry must never be able to crash the whole app.
void InitImpl() {
    g_entitiesById.clear();

    Log::Info("[INFO] EntityRegistry: reading " + ENTITIES_JSON_PATH);

    std::ifstream file(ENTITIES_JSON_PATH);
    if (!file) {
        Log::Error("[ERROR] EntityRegistry: couldn't open " + ENTITIES_JSON_PATH
            + " — entity lookups will find nothing this run.");
        return;
    }

    json entitiesJson;
    try {
        file >> entitiesJson;
    } catch (const std::exception& e) {
        Log::Error(std::string("[ERROR] EntityRegistry: failed to parse entities.json: ") + e.what());
        return;
    }

    Log::Info("[INFO] EntityRegistry: parsed " + std::to_string(entitiesJson.size()) + " entity entries.");

    for (const json& e : entitiesJson) {
        try {
            if (!e.contains("id") || !e["id"].is_number()) continue;

            EntityTypeInfo info;
            info.name = e.value("name", std::string());
            info.displayName = e.value("displayName", std::string());
            info.width = e.value("width", 0.0f);
            info.height = e.value("height", 0.0f);
            info.category = e.value("category", std::string());

            g_entitiesById.emplace(e["id"].get<uint32_t>(), std::move(info));
        } catch (const std::exception& e2) {
            Log::Error(std::string("[ERROR] EntityRegistry: skipping a malformed entity entry: ") + e2.what());
        }
    }

    Log::Info("[INFO] EntityRegistry: loaded minecraft-data " + std::string(MC_DATA_VERSION) + " — "
        + std::to_string(g_entitiesById.size()) + " entity types.");
}

} // namespace

void Init() {
    try {
        InitImpl();
    } catch (const std::exception& e) {
        Log::Error(std::string("[ERROR] EntityRegistry: Init() failed unexpectedly, entity lookups will find "
            "nothing this run: ") + e.what());
    }
}

const EntityTypeInfo* Lookup(uint32_t protocolTypeId) {
    auto it = g_entitiesById.find(protocolTypeId);
    return it != g_entitiesById.end() ? &it->second : nullptr;
}

} // namespace Volcano::EntityRegistry
