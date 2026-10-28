#include "EntityRegistry.hpp"
#include "Logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;

namespace Volcano::EntityRegistry {

namespace {

// Kept in sync with BlockRegistry's pinned minecraft-data version and
// PROTOCOL_VERSION (NetworkClient.hpp) — all three must agree.
constexpr const char* MC_DATA_VERSION = "26.1";
const std::string ENTITIES_JSON_PATH = std::string("resources/minecraft-data/data/pc/") + MC_DATA_VERSION + "/entities.json";

std::unordered_map<uint32_t, EntityTypeInfo> g_entitiesById;

// Picks EntityRenderer's procedural shape for a type by name — minecraft-
// data has no shape/model hint of its own (its "category" field is far too
// coarse, e.g. "Passive mobs" covers both cows and bats), so this is a
// curated list against the handful of shapes EntityRenderer actually knows
// how to build. `entityKind` is minecraft-data's "type" field, checked first
// as a cheap correctness net: only living things (animal/hostile/player/
// mob/living/water_creature/ambient) are ever eligible, so a name collision
// with something inanimate (items, projectiles, vehicles) can't accidentally
// pick up a shape. Anything not listed below keeps the flat placeholder cube.
ShapeKind ClassifyShape(const std::string& name, const std::string& entityKind)
{
    // Inanimate, so checked before the living-only gate below.
    if (name == "falling_block" || name == "tnt") return ShapeKind::Block;

    static const std::unordered_set<std::string> LIVING_KINDS = {
        "player", "mob", "living", "animal", "passive", "hostile", "water_creature", "ambient"
    };
    if (!LIVING_KINDS.count(entityKind)) return ShapeKind::Cube;

    // Kept in sync with EntityRenderer.cpp's SkinForEntityType — that's what
    // actually resolves a bound skin texture for each of these; a name added
    // here with no matching skin still renders (falls back to the flat cube
    // for that one type, see EntityRenderer::RecordDraw), it just won't look
    // humanoid until a skin is wired up too.
    static const std::unordered_set<std::string> HUMANOID_NAMES = {
        "player", "mannequin", "zombie", "skeleton"
    };
    if (HUMANOID_NAMES.count(name)) return ShapeKind::Humanoid;

    if (name == "iron_golem") return ShapeKind::Golem;

    // Real vanilla-derived proportions (see PigModel.hpp/CowModel.hpp) — the
    // two species the generic Quadruped box visibly looked wrong for.
    // Mooshroom shares the cow model in vanilla too.
    if (name == "pig") return ShapeKind::Pig;
    if (name == "cow" || name == "mooshroom") return ShapeKind::Cow;

    static const std::unordered_set<std::string> QUADRUPED_NAMES = {
        "sheep", "wolf", "horse", "donkey", "mule", "llama", "trader_llama", "goat"
    };
    if (QUADRUPED_NAMES.count(name)) return ShapeKind::Quadruped;

    return ShapeKind::Cube;
}

// Split out so the public Init() below can wrap it in a catch-all — a single
// malformed entity entry must never be able to crash the whole app.
void InitImpl() {
    g_entitiesById.clear();

    Log::Info("EntityRegistry: reading " + ENTITIES_JSON_PATH);

    std::ifstream file(ENTITIES_JSON_PATH);
    if (!file) {
        Log::Error("EntityRegistry: couldn't open " + ENTITIES_JSON_PATH
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

    Log::Info("EntityRegistry: parsed " + std::to_string(entitiesJson.size()) + " entity entries.");

    for (const json& e : entitiesJson) {
        try {
            if (!e.contains("id") || !e["id"].is_number()) continue;

            EntityTypeInfo info;
            info.name = e.value("name", std::string());
            info.displayName = e.value("displayName", std::string());
            info.width = e.value("width", 0.0f);
            info.height = e.value("height", 0.0f);
            info.category = e.value("category", std::string());
            info.entityKind = e.value("type", std::string());
            info.shape = ClassifyShape(info.name, info.entityKind);

            g_entitiesById.emplace(e["id"].get<uint32_t>(), std::move(info));
        } catch (const std::exception& e2) {
            Log::Error(std::string("[ERROR] EntityRegistry: skipping a malformed entity entry: ") + e2.what());
        }
    }

    Log::Info("EntityRegistry: loaded minecraft-data " + std::string(MC_DATA_VERSION) + " — "
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
