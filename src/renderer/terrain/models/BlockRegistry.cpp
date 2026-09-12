#include "BlockRegistry.hpp"
#include "Logger.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace Volcano::BlockRegistry {

namespace {

// minecraft-data has no data folder for this client's protocol version (776,
// Minecraft 26.2) yet — 26.1 is the closest available, and block/entity
// registries don't change between adjacent releases. Bump this (and nothing
// else) once a matching folder exists.
constexpr const char* MC_DATA_VERSION = "26.1";
const std::string BLOCKS_JSON_PATH = std::string("resources/minecraft-data/data/pc/") + MC_DATA_VERSION + "/blocks.json";
const std::string ASSETS_ROOT = "resources/minecraft/assets/minecraft";

// One property declared on a block in blocks.json's "states" array.
struct StateProp {
    std::string name;
    std::string type; // "enum", "bool", "int", ...
    int32_t numValues = 0;
    std::vector<std::string> values; // populated for "enum"; "bool" has no explicit values array.
};

struct BlockDef {
    std::string name;
    int32_t minStateId = 0;
    int32_t maxStateId = 0;
    int32_t defaultState = 0;
    std::vector<StateProp> states;
};

// A block's decoded property values for one specific state id, always kept
// sorted by property name (std::map) so it can be compared directly against
// a blockstate JSON variant key's parsed property set.
using DecodedProps = std::map<std::string, std::string>;

// One fully-resolved block/model, after walking its "parent" chain: textures
// merged child-over-parent (values may still be "#variable" references into
// this same map), and elements taken from this model if it defines any,
// else inherited from the nearest ancestor that does.
struct ResolvedModel {
    std::unordered_map<std::string, std::string> textures;
    json elements = json::array();
    bool hasElements = false;
};

// Interned face-texture-name tuples, ChunkMesher's face order: 0=Up, 1=Down,
// 2=North, 3=South, 4=East, 5=West. Index 0 is reserved for "nothing to
// draw" (all-empty tuple) — true air, or any block this registry couldn't
// resolve to a full single-cube model this pass.
using FaceNames = std::array<std::string, 6>;

std::vector<FaceNames> g_visualFaceNames;
std::map<FaceNames, uint16_t> g_visualIntern;
std::vector<uint16_t> g_stateIdToVisual;
std::unordered_map<std::string, uint16_t> g_nameToVisual;
int32_t g_maxLoadedStateId = -1;

// Non-cube visuals aren't deduplicated/interned the way full-cube visuals
// are above (NonCubeVisual holds vectors/strings, not a small fixed-size
// key) — every resolved state just appends a new table entry.
std::vector<uint16_t> g_stateIdToNonCubeVisual;
std::vector<NonCubeVisual> g_nonCubeVisualTable;

std::unordered_map<std::string, ResolvedModel> g_modelCache;

std::mutex g_loggedMutex;
std::unordered_set<int32_t> g_loggedOutOfRangeIds;

std::string StripNamespace(const std::string& id) {
    size_t colon = id.find(':');
    return colon == std::string::npos ? id : id.substr(colon + 1);
}

// Reduces a texture reference ("minecraft:block/stone", "block/stone", or a
// bare "stone") to the bare stem TextureManager's PNG-stem-keyed lookup
// expects.
std::string TextureStem(const std::string& ref) {
    std::string value = StripNamespace(ref);
    size_t slash = value.find_last_of('/');
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

// Follows a chain of "#variable" references through a model's merged
// textures map until it hits a concrete path, or the chain breaks/loops.
bool ResolveTextureRef(const std::unordered_map<std::string, std::string>& textures,
        std::string ref, std::string& outStem) {
    for (int depth = 0; depth < 8; depth++) {
        if (ref.empty() || ref[0] != '#') {
            outStem = TextureStem(ref);
            return true;
        }
        auto it = textures.find(ref.substr(1));
        if (it == textures.end()) return false;
        ref = it->second;
    }
    return false; // Cyclic or too-deep reference chain — shouldn't happen in valid data.
}

// Loads and resolves a model's full parent chain (cached by stripped model
// id, e.g. "block/cube_column"). Recursion resolves the parent entirely
// before this frame's own emplace() runs, so the returned reference stays
// valid even though other entries get inserted afterward (unordered_map
// only invalidates iterators on insert/rehash, never existing references).
const ResolvedModel& ResolveModel(const std::string& modelIdRaw, int depth = 0) {
    std::string modelId = StripNamespace(modelIdRaw);

    auto cached = g_modelCache.find(modelId);
    if (cached != g_modelCache.end()) return cached->second;

    ResolvedModel result;

    if (depth < 16) { // Guards against a malformed/cyclic parent chain.
        fs::path path = fs::path(ASSETS_ROOT) / "models" / (modelId + ".json");
        std::ifstream file(path);
        if (file) {
            try {
                json j;
                file >> j;

                if (j.contains("parent") && j["parent"].is_string()) {
                    const ResolvedModel& parent = ResolveModel(j["parent"].get<std::string>(), depth + 1);
                    result.textures = parent.textures;
                    result.elements = parent.elements;
                    result.hasElements = parent.hasElements;
                }

                if (j.contains("textures") && j["textures"].is_object()) {
                    for (const auto& [key, value] : j["textures"].items()) {
                        if (value.is_string()) result.textures[key] = value.get<std::string>();
                    }
                }

                if (j.contains("elements") && j["elements"].is_array()) {
                    result.elements = j["elements"];
                    result.hasElements = true;
                }
            } catch (const std::exception& e) {
                Log::Error("[ERROR] BlockRegistry: failed to parse model " + modelId + ": " + e.what());
            }
        }
    }

    auto [inserted, _] = g_modelCache.emplace(std::move(modelId), std::move(result));
    return inserted->second;
}

// A model is renderable by the current (cube-only) greedy mesher exactly
// when it resolves to a single element spanning the full block (0,0,0) to
// (16,16,16) with all six faces present. Stairs/slabs/fences/plants/etc.
// either have multiple elements or a partial extent and fail this — by
// design, they're excluded this pass (visual id 0) rather than rendered
// wrong-shaped; see BlockRegistry.hpp.
bool IsFullCube(const json& elements) {
    if (!elements.is_array() || elements.size() != 1) return false;
    const json& elem = elements[0];
    if (!elem.contains("from") || !elem.contains("to") || !elem.contains("faces")) return false;

    auto isCorner = [](const json& v, double x, double y, double z) {
        if (!v.is_array() || v.size() != 3) return false;
        return std::abs(v[0].get<double>() - x) < 0.001
            && std::abs(v[1].get<double>() - y) < 0.001
            && std::abs(v[2].get<double>() - z) < 0.001;
    };
    if (!isCorner(elem["from"], 0, 0, 0) || !isCorner(elem["to"], 16, 16, 16)) return false;

    const json& faces = elem["faces"];
    if (!faces.is_object()) return false;
    for (const char* dir : { "down", "up", "north", "south", "east", "west" }) {
        if (!faces.contains(dir) || !faces[dir].contains("texture")) return false;
    }
    return true;
}

// Decodes a state id's offset within its block's [minStateId, maxStateId]
// range into concrete property values. Properties pack with the
// last-declared one varying fastest (verified against grass_block's single
// "snowy" bool: offset 0 -> snowy=true, offset 1 -> snowy=false, matching
// defaultState == maxStateId == snowy=false) — so states are walked in
// reverse, mixed-radix style. "bool" properties have no explicit "values"
// array in blocks.json; their implicit order is [true, false].
DecodedProps DecodeProps(const std::vector<StateProp>& states, int32_t offset) {
    DecodedProps result;
    int32_t remaining = offset;
    for (auto it = states.rbegin(); it != states.rend(); ++it) {
        const StateProp& prop = *it;
        int32_t divisor = prop.numValues > 0 ? prop.numValues : 1;
        int32_t index = remaining % divisor;
        remaining /= divisor;

        std::string value;
        if (prop.type == "bool") {
            value = (index == 0) ? "true" : "false";
        } else if (index < static_cast<int32_t>(prop.values.size())) {
            value = prop.values[static_cast<size_t>(index)];
        } else {
            value = std::to_string(index);
        }
        result[prop.name] = value;
    }
    return result;
}

// Parses a blockstate variant key ("" or "prop=val,prop2=val2", in any
// order) into the same shape DecodeProps produces, for direct comparison.
DecodedProps ParseVariantKey(const std::string& key) {
    DecodedProps result;
    size_t pos = 0;
    while (pos < key.size()) {
        size_t comma = key.find(',', pos);
        std::string part = key.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos) result[part.substr(0, eq)] = part.substr(eq + 1);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return result;
}

// Finds the model id for the variant matching `props` in a "variants"-style
// blockstate JSON. Ignores "multipart" blockstates entirely (fences, walls,
// redstone dust, ...) — those are non-cube by construction and excluded
// this pass. A variant's value may be a single object or an array of
// (rotation-only) alternatives; element 0 is used either way since which
// texture is used never depends on the rotation choice.
bool FindVariantModel(const json& blockstateJson, const DecodedProps& props, std::string& outModelId) {
    if (!blockstateJson.contains("variants") || !blockstateJson["variants"].is_object()) return false;

    for (const auto& [key, value] : blockstateJson["variants"].items()) {
        if (ParseVariantKey(key) != props) continue;

        const json& chosen = value.is_array() ? value[0] : value;
        if (!chosen.contains("model") || !chosen["model"].is_string()) return false;
        outModelId = chosen["model"].get<std::string>();
        return true;
    }
    return false;
}

uint16_t InternVisual(const FaceNames& faces) {
    auto it = g_visualIntern.find(faces);
    if (it != g_visualIntern.end()) return it->second;

    uint16_t id = static_cast<uint16_t>(g_visualFaceNames.size());
    g_visualFaceNames.push_back(faces);
    g_visualIntern.emplace(faces, id);
    return id;
}

// Resolves one state id to a visual id, or 0 if the variant/model/texture
// chain doesn't fully resolve to a renderable full cube. Any unexpected
// shape in the resource-pack JSON (a "texture" value that isn't a string,
// etc.) is treated the same as "doesn't resolve" rather than thrown —
// callers must never have a single odd block's data take down Init() for
// every other block.
uint16_t ResolveStateVisual(const BlockDef& def, const json& blockstateJson, int32_t stateId) {
    try {
        DecodedProps props = DecodeProps(def.states, stateId - def.minStateId);

        std::string modelId;
        if (!FindVariantModel(blockstateJson, props, modelId)) return 0;

        const ResolvedModel& model = ResolveModel(modelId);
        if (!model.hasElements || !IsFullCube(model.elements)) return 0;

        static const char* dirs[6] = { "up", "down", "north", "south", "east", "west" }; // ChunkMesher's 0..5 order.
        const json& faces = model.elements[0]["faces"];

        FaceNames faceNames;
        for (int f = 0; f < 6; f++) {
            const json& texField = faces[dirs[f]]["texture"];
            if (!texField.is_string()) return 0;
            if (!ResolveTextureRef(model.textures, texField.get<std::string>(), faceNames[static_cast<size_t>(f)])) return 0;
        }

        return InternVisual(faceNames);
    } catch (const std::exception& e) {
        Log::Error("[ERROR] BlockRegistry: failed to resolve " + def.name + " state " + std::to_string(stateId)
            + ": " + e.what());
        return 0;
    }
}

// Vanilla has no per-block "is this translucent" field in minecraft-data or
// the blockstate/model JSON we already parse — real rendering-layer
// assignment lives in code, not data. Rather than shipping a huge hardcoded
// per-version block list, this covers the name patterns the task actually
// asked for (glass, slime, honey) plus the other common vanilla translucent
// full cubes; anything else that happens to be a full cube renders through
// the normal opaque path exactly as before.
bool IsTransparentCubeName(const std::string& name) {
    if (name.find("glass") != std::string::npos) return true;
    static const std::unordered_set<std::string> named = {
        "slime_block", "honey_block", "ice", "frosted_ice",
    };
    return named.contains(name);
}

uint16_t InternNonCubeVisual(NonCubeVisual&& visual) {
    uint16_t id = static_cast<uint16_t>(g_nonCubeVisualTable.size());
    g_nonCubeVisualTable.push_back(std::move(visual));
    return id;
}

// Resolves one state id to a non-cube visual, for any state that
// ResolveStateVisual above couldn't place on the opaque full-cube path.
// Shares FindVariantModel/ResolveModel with the opaque path so both read the
// exact same resolved textures/elements — only the classification differs.
uint16_t ResolveStateNonCubeVisual(const BlockDef& def, const json& blockstateJson, int32_t stateId) {
    try {
        DecodedProps props = DecodeProps(def.states, stateId - def.minStateId);

        std::string modelId;
        if (!FindVariantModel(blockstateJson, props, modelId)) return 0;

        const ResolvedModel& model = ResolveModel(modelId);
        if (!model.hasElements) return 0;

        static const char* dirs[6] = { "up", "down", "north", "south", "east", "west" }; // ChunkMesher's 0..5 order.

        // Vanilla's block/cross template model leaves a "cross" texture
        // variable unresolved in its own file; only a child model (grass,
        // flowers, saplings, ...) that extends it defines what "#cross"
        // actually points to — so seeing a resolved "cross" key here means
        // this block extends that template, without needing to track model
        // identity through the parent chain (ResolveModel deliberately
        // doesn't keep it — see its own comment).
        auto crossTex = model.textures.find("cross");
        if (crossTex != model.textures.end()) {
            std::string stem;
            if (!ResolveTextureRef(model.textures, "#cross", stem)) return 0;

            NonCubeVisual visual;
            visual.shape = NonCubeShape::Cross;
            visual.collidable = false;
            visual.crossTexture = stem;
            return InternNonCubeVisual(std::move(visual));
        }

        bool fullCubeShape = IsFullCube(model.elements);
        bool transparentCube = fullCubeShape && IsTransparentCubeName(def.name);
        // A full cube that ISN'T one of the known translucent names already
        // resolved via the opaque path (or failed for an unrelated reason,
        // e.g. a missing texture) — either way, not this function's job.
        if (fullCubeShape && !transparentCube) return 0;

        NonCubeVisual visual;
        visual.shape = transparentCube ? NonCubeShape::TransparentCube : NonCubeShape::Partial;
        visual.collidable = true;

        for (const json& elem : model.elements) {
            if (!elem.contains("from") || !elem.contains("to") || !elem.contains("faces")) continue;
            const json& from = elem["from"];
            const json& to = elem["to"];
            if (!from.is_array() || from.size() != 3 || !to.is_array() || to.size() != 3) continue;

            NonCubeElement element;
            element.from = glm::vec3(from[0].get<double>(), from[1].get<double>(), from[2].get<double>());
            element.to = glm::vec3(to[0].get<double>(), to[1].get<double>(), to[2].get<double>());

            const json& faces = elem["faces"];
            if (faces.is_object()) {
                for (int f = 0; f < 6; f++) {
                    if (!faces.contains(dirs[f]) || !faces[dirs[f]].contains("texture")) continue;
                    const json& texField = faces[dirs[f]]["texture"];
                    if (!texField.is_string()) continue;
                    std::string stem;
                    if (ResolveTextureRef(model.textures, texField.get<std::string>(), stem)) {
                        element.faceTextures[static_cast<size_t>(f)] = stem;
                    }
                }
            }
            visual.elements.push_back(std::move(element));
        }

        if (visual.elements.empty()) return 0;
        return InternNonCubeVisual(std::move(visual));
    } catch (const std::exception& e) {
        Log::Error("[ERROR] BlockRegistry: failed to resolve non-cube visual for " + def.name
            + " state " + std::to_string(stateId) + ": " + e.what());
        return 0;
    }
}

// The real body of Init(), split out so the public entry point below can
// wrap it in one last catch-all — nothing in here should be able to bring
// the whole app down over a single bad block/file; anything that does is a
// bug in this function, not a reason to crash the client.
void InitImpl() {
    g_visualFaceNames.clear();
    g_visualIntern.clear();
    g_nameToVisual.clear();
    g_stateIdToVisual.clear();
    g_stateIdToNonCubeVisual.clear();
    g_nonCubeVisualTable.clear();
    g_modelCache.clear();
    g_maxLoadedStateId = -1;

    g_visualFaceNames.push_back(FaceNames{}); // Visual id 0: nothing to draw.
    g_nonCubeVisualTable.push_back(NonCubeVisual{}); // Non-cube visual id 0: nothing to draw.

    Log::Info("[INFO] BlockRegistry: reading " + BLOCKS_JSON_PATH
        + " (cwd: " + fs::current_path().string() + ")");

    if (!fs::exists(ASSETS_ROOT)) {
        Log::Error("[ERROR] BlockRegistry: resource-pack assets root not found: " + ASSETS_ROOT
            + " — every block will render as nothing this run.");
    }

    std::ifstream blocksFile(BLOCKS_JSON_PATH);
    if (!blocksFile) {
        Log::Error("[ERROR] BlockRegistry: couldn't open " + BLOCKS_JSON_PATH
            + " — every block will render as nothing this run.");
        return;
    }

    json blocksJson;
    try {
        blocksFile >> blocksJson;
    } catch (const std::exception& e) {
        Log::Error(std::string("[ERROR] BlockRegistry: failed to parse blocks.json: ") + e.what());
        return;
    }

    Log::Info("[INFO] BlockRegistry: parsed " + std::to_string(blocksJson.size())
        + " block definitions, resolving textures against " + ASSETS_ROOT + "...");

    std::vector<BlockDef> blockDefs;
    blockDefs.reserve(blocksJson.size());
    int32_t maxStateId = 0;

    for (const json& b : blocksJson) {
        try {
            BlockDef def;
            def.name = b.value("name", std::string());
            def.minStateId = b.value("minStateId", 0);
            def.maxStateId = b.value("maxStateId", def.minStateId);
            def.defaultState = b.value("defaultState", def.minStateId);

            if (b.contains("states") && b["states"].is_array()) {
                for (const json& s : b["states"]) {
                    StateProp prop;
                    prop.name = s.value("name", std::string());
                    prop.type = s.value("type", std::string());
                    prop.numValues = s.value("num_values", 0);
                    if (s.contains("values") && s["values"].is_array()) {
                        for (const json& v : s["values"]) {
                            prop.values.push_back(v.is_string() ? v.get<std::string>() : v.dump());
                        }
                    }
                    def.states.push_back(std::move(prop));
                }
            }

            maxStateId = std::max(maxStateId, def.maxStateId);
            blockDefs.push_back(std::move(def));
        } catch (const std::exception& e) {
            Log::Error(std::string("[ERROR] BlockRegistry: skipping a malformed block entry in blocks.json: ") + e.what());
        }
    }

    g_stateIdToVisual.assign(static_cast<size_t>(maxStateId) + 1, 0);
    g_stateIdToNonCubeVisual.assign(static_cast<size_t>(maxStateId) + 1, 0);
    g_maxLoadedStateId = maxStateId;

    int resolvedBlocks = 0, excludedBlocks = 0, nonCubeResolvedBlocks = 0;

    for (const BlockDef& def : blockDefs) {
        try {
            fs::path bsPath = fs::path(ASSETS_ROOT) / "blockstates" / (def.name + ".json");
            std::ifstream bsFile(bsPath);

            json bsJson;
            bool hasVariants = false;
            if (bsFile) {
                try {
                    bsFile >> bsJson;
                    hasVariants = bsJson.contains("variants") && bsJson["variants"].is_object();
                } catch (const std::exception&) {
                    hasVariants = false;
                }
            }

            if (!hasVariants) {
                // No blockstate file (fluids, cave_air/void_air, ...), or a
                // "multipart" blockstate (fences/walls/redstone dust/...) —
                // both are non-cube by construction. Whole range stays visual 0.
                excludedBlocks++;
                continue;
            }

            bool anyResolved = false;
            bool anyNonCubeResolved = false;
            for (int32_t stateId = def.minStateId; stateId <= def.maxStateId; stateId++) {
                uint16_t visual = ResolveStateVisual(def, bsJson, stateId);
                g_stateIdToVisual[static_cast<size_t>(stateId)] = visual;

                if (visual == 0) {
                    uint16_t nonCubeVisual = ResolveStateNonCubeVisual(def, bsJson, stateId);
                    g_stateIdToNonCubeVisual[static_cast<size_t>(stateId)] = nonCubeVisual;
                    anyNonCubeResolved |= (nonCubeVisual != 0);
                }

                anyResolved |= (visual != 0);
            }
            if (anyNonCubeResolved) nonCubeResolvedBlocks++;

            if (anyResolved) {
                resolvedBlocks++;
                uint16_t defaultVisual = (def.defaultState >= def.minStateId && def.defaultState <= def.maxStateId)
                    ? g_stateIdToVisual[static_cast<size_t>(def.defaultState)] : 0;
                if (defaultVisual == 0) {
                    for (int32_t stateId = def.minStateId; stateId <= def.maxStateId; stateId++) {
                        if (g_stateIdToVisual[static_cast<size_t>(stateId)] != 0) {
                            defaultVisual = g_stateIdToVisual[static_cast<size_t>(stateId)];
                            break;
                        }
                    }
                }
                g_nameToVisual[def.name] = defaultVisual;
            } else {
                excludedBlocks++;
            }
        } catch (const std::exception& e) {
            Log::Error("[ERROR] BlockRegistry: skipping block \"" + def.name + "\": " + e.what());
            excludedBlocks++;
        }
    }

    Log::Info("[INFO] BlockRegistry: loaded minecraft-data " + std::string(MC_DATA_VERSION) + " — "
        + std::to_string(resolvedBlocks) + " blocks textured as full cubes, "
        + std::to_string(nonCubeResolvedBlocks) + " textured as cross/partial/transparent-cube visuals, "
        + std::to_string(excludedBlocks) + " excluded this pass (fluid/multipart/unresolvable), "
        + std::to_string(g_visualFaceNames.size() - 1) + " distinct full-cube visuals, "
        + std::to_string(g_nonCubeVisualTable.size() - 1) + " distinct non-cube visuals.");
}

} // namespace

void Init() {
    try {
        InitImpl();
    } catch (const std::exception& e) {
        Log::Error(std::string("[ERROR] BlockRegistry: Init() failed unexpectedly, every block will render "
            "as nothing this run: ") + e.what());
    }
}

uint16_t MapStateId(int32_t stateId) {
    if (stateId >= 0 && stateId <= g_maxLoadedStateId) {
        return g_stateIdToVisual[static_cast<size_t>(stateId)];
    }

    {
        std::lock_guard lock(g_loggedMutex);
        if (g_loggedOutOfRangeIds.insert(stateId).second) {
            Log::Info("[NET] Block state id " + std::to_string(stateId)
                + " is outside the loaded block registry's range (0.." + std::to_string(g_maxLoadedStateId)
                + ") — possible protocol/minecraft-data version mismatch. Rendering as nothing.");
        }
    }
    return 0;
}

const std::string& GetFaceTextureName(uint16_t visualId, int faceIndex) {
    static const std::string fallback = "stone";
    if (visualId >= g_visualFaceNames.size() || faceIndex < 0 || faceIndex > 5) return fallback;

    const std::string& name = g_visualFaceNames[visualId][static_cast<size_t>(faceIndex)];
    return name.empty() ? fallback : name;
}

uint16_t VisualIdForName(const std::string& blockName) {
    auto it = g_nameToVisual.find(blockName);
    return it != g_nameToVisual.end() ? it->second : 0;
}

uint16_t MapStateIdNonCube(int32_t stateId) {
    if (stateId >= 0 && stateId <= g_maxLoadedStateId) {
        return g_stateIdToNonCubeVisual[static_cast<size_t>(stateId)];
    }
    return 0; // Out-of-range ids are already logged once by MapStateId for the same state id.
}

const NonCubeVisual& GetNonCubeVisual(uint16_t nonCubeVisualId) {
    static const NonCubeVisual empty{};
    if (nonCubeVisualId == 0 || nonCubeVisualId >= g_nonCubeVisualTable.size()) return empty;
    return g_nonCubeVisualTable[nonCubeVisualId];
}

} // namespace Volcano::BlockRegistry
