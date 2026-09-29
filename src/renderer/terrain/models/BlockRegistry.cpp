#include "BlockRegistry.hpp"
#include "Logger.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
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

// Matches PROTOCOL_VERSION (NetworkClient.hpp) exactly — this client
// declares 26.1, not 26.2, specifically because 26.1 is the newest version
// minecraft-data has real block/entity id tables for.
constexpr const char* MC_DATA_VERSION = "26.1";
const std::string BLOCKS_JSON_PATH = std::string("resources/minecraft-data/data/pc/") + MC_DATA_VERSION + "/blocks.json";
const std::string COLLISION_SHAPES_JSON_PATH = std::string("resources/minecraft-data/data/pc/") + MC_DATA_VERSION + "/blockCollisionShapes.json";
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

    // Mining-time inputs (blocks.json's own fields) — see MapStateIdBreakTicks.
    float hardness = 0.0f;
    bool diggable = true;
    bool requiresTool = false; // harvestTools present and non-empty.
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

// Collision data from blockCollisionShapes.json — see MapStateIdCollisionShape.
// g_collisionShapes is indexed directly by the file's shape id (0 = empty).
// Empty g_stateIdToCollisionShape means the file didn't load.
std::vector<uint16_t> g_stateIdToCollisionShape;
std::vector<CollisionBoxes> g_collisionShapes;

// Break-time table — see MapStateIdBreakTicks. -1 (unbreakable) is the
// default fill so an out-of-range id already reads as "can't break" rather
// than "instant", same fail-safe direction as g_stateIdToCollisionShape's
// UNKNOWN default.
std::vector<int32_t> g_stateIdToBreakTicks;

std::unordered_map<std::string, int32_t> g_nameToDefaultState;

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
                        if (value.is_string()) {
                            result.textures[key] = value.get<std::string>();
                        } else if (value.is_object() && value.contains("sprite") && value["sprite"].is_string()) {
                            // Newer texture-reference form glass/stained glass use for
                            // translucency-sort metadata ({"sprite": "...",
                            // "force_translucent": true}) — the metadata doesn't matter
                            // here, only the sprite path. Silently dropping this whole
                            // entry (the previous behavior) broke every #variable chain
                            // through it, which is why glass rendered/collided as nothing.
                            result.textures[key] = value["sprite"].get<std::string>();
                        }
                    }
                }

                if (j.contains("elements") && j["elements"].is_array()) {
                    result.elements = j["elements"];
                    result.hasElements = true;
                }
            } catch (const std::exception& e) {
                Log::Error("BlockRegistry: failed to parse model " + modelId + ": " + e.what());
            }
        }
    }

    auto [inserted, _] = g_modelCache.emplace(std::move(modelId), std::move(result));
    return inserted->second;
}

// Whether one element (not the whole model) spans the full block (0,0,0) to
// (16,16,16) with all six faces present — the shape the cube-only greedy
// mesher can render.
bool IsFullCubeElement(const json& elem) {
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

// A model is renderable by the current (cube-only) greedy mesher exactly
// when it resolves to a single element spanning the full block. Stairs/
// slabs/fences/plants/etc. either have multiple elements or a partial
// extent and fail this — by design, they're excluded this pass (visual id
// 0) rather than rendered wrong-shaped; see BlockRegistry.hpp.
bool IsFullCube(const json& elements) {
    return elements.is_array() && elements.size() == 1 && IsFullCubeElement(elements[0]);
}

// Finds the first full-cube-shaped element in a model with MULTIPLE
// elements — grass_block (and anything else using the same tinted-overlay
// technique: a base cube plus a separate biome-tinted overlay element on
// the same footprint) has 2 elements and so fails IsFullCube's strict
// single-element check above, even though it's visually still just an
// opaque full cube; this second element (the tint overlay) isn't something
// the current single-texture-per-face greedy mesher can layer in anyway, so
// using just the base element's own faces — silently dropping the overlay —
// is the closest this pass can render it, and is a lot better than the
// whole block excluding to nothing (which is what "what happened to the
// grass" turned out to be).
const json* FindOpaqueCubeElement(const json& elements) {
    if (!elements.is_array()) return nullptr;
    for (const json& elem : elements) {
        if (IsFullCubeElement(elem)) return &elem;
    }
    return nullptr;
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

// True when every property named in `variantProps` (parsed from one variant
// key) matches the corresponding value in `props` (the block's full decoded
// state) — NOT full map equality. Vanilla blockstate JSON only lists the
// properties that actually change which model is picked: stairs/slabs/
// fences/... all have a real "waterlogged" state (it's in blocks.json, so
// DecodeProps always includes it in `props`), but no variant key ever
// mentions "waterlogged" — water is layered on separately in real Minecraft,
// not part of the model choice. Requiring exact equality against the FULL
// prop set meant `ParseVariantKey(key)` (missing "waterlogged") could never
// equal `props` (which always has it), so every state of every waterlogged-
// capable block failed to match here and rendered as nothing — the actual
// cause of stairs/slabs/etc. showing up as holes, not anything specific to
// their shape.
bool VariantKeyMatches(const DecodedProps& variantProps, const DecodedProps& props) {
    for (const auto& [key, value] : variantProps) {
        auto it = props.find(key);
        if (it == props.end() || it->second != value) return false;
    }
    return true;
}

// Finds the model id for the variant matching `props` in a "variants"-style
// blockstate JSON, plus that variant's model-space rotation (degrees, always
// a multiple of 90 in vanilla data) — "y" turns a stair/etc.'s baked-in
// facing into the other three (e.g. polished_deepslate_stairs.json's base
// model faces east; y=90/180/270 give south/west/north), "x" flips it
// (half=top uses x=180 to turn a bottom stair upside down). Ignores
// "multipart" blockstates entirely — those never have a "variants" key, so
// this always returns false for them; see ResolveMultipartNonCubeVisual for
// the separate path that resolves fences/walls (the one multipart shape
// this pass handles). A variant's value may be a single object or an array
// of (rotation-only) alternatives; element 0 is used either way since which
// texture is used never depends on the rotation choice.
bool FindVariantModel(const json& blockstateJson, const DecodedProps& props, std::string& outModelId,
        int& outXDeg, int& outYDeg) {
    outXDeg = 0;
    outYDeg = 0;
    if (!blockstateJson.contains("variants") || !blockstateJson["variants"].is_object()) return false;

    for (const auto& [key, value] : blockstateJson["variants"].items()) {
        if (!VariantKeyMatches(ParseVariantKey(key), props)) continue;

        const json& chosen = value.is_array() ? value[0] : value;
        if (!chosen.contains("model") || !chosen["model"].is_string()) return false;
        outModelId = chosen["model"].get<std::string>();
        if (chosen.contains("x") && chosen["x"].is_number_integer()) outXDeg = chosen["x"].get<int>();
        if (chosen.contains("y") && chosen["y"].is_number_integer()) outYDeg = chosen["y"].get<int>();
        return true;
    }
    return false;
}

// cos/sin for a rotation restricted to a multiple of 90 degrees, as exact
// integers — avoids the float noise plain std::cos/sin(radians(90)) would
// introduce (cos(90 deg) isn't exactly 0), which would otherwise leave
// rotated box corners very slightly off their intended 0..16 grid values.
void SinCos90(int deg, int& outCos, int& outSin) {
    switch (((deg % 360) + 360) % 360) {
        case 90:  outCos = 0;  outSin = 1;  break;
        case 180: outCos = -1; outSin = 0;  break;
        case 270: outCos = 0;  outSin = -1; break;
        default:  outCos = 1;  outSin = 0;  break; // 0, or anything non-90-aligned (shouldn't appear in vanilla data)
    }
}

// Rotates a direction (no translation) by xDeg around the X axis, then yDeg
// around the Y axis — the same order (x then y) a blockstate variant's own
// "x"/"y" fields are applied in. Used for both element corners (as an offset
// from the block center) and face normals (to figure out which face index a
// rotated face belongs to).
glm::vec3 RotateDirection(glm::vec3 v, int xDeg, int yDeg) {
    if (xDeg != 0) {
        int cs, sn;
        SinCos90(xDeg, cs, sn);
        float y = v.y * static_cast<float>(cs) - v.z * static_cast<float>(sn);
        float z = v.y * static_cast<float>(sn) + v.z * static_cast<float>(cs);
        v.y = y;
        v.z = z;
    }
    if (yDeg != 0) {
        int cs, sn;
        SinCos90(yDeg, cs, sn);
        float x = v.x * static_cast<float>(cs) - v.z * static_cast<float>(sn);
        float z = v.x * static_cast<float>(sn) + v.z * static_cast<float>(cs);
        v.x = x;
        v.z = z;
    }
    return v;
}

// Rotates a model-space point (0..16 units) around the block's center
// (8,8,8) — used for element box corners.
glm::vec3 RotatePoint(const glm::vec3& p, int xDeg, int yDeg) {
    const glm::vec3 center(8.0f, 8.0f, 8.0f);
    return RotateDirection(p - center, xDeg, yDeg) + center;
}

// ChunkMesher's face order/normals: 0=Up, 1=Down, 2=North(-Z), 3=South(+Z),
// 4=East(+X), 5=West(-X).
constexpr glm::vec3 FACE_NORMALS[6] = {
    {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f},
    {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
};

int FaceIndexFromNormal(const glm::vec3& n) {
    for (int i = 0; i < 6; i++) {
        if (glm::dot(n, FACE_NORMALS[static_cast<size_t>(i)]) > 0.5f) return i;
    }
    return -1; // Shouldn't happen for an axis-aligned normal rotated by a multiple of 90 degrees.
}

// Rotates one already-built element in place by the variant's x/y (see
// FindVariantModel) — both the box itself and which physical direction each
// of its faces now points, so e.g. a stair's "north" face (as baked into the
// unrotated model file) ends up keyed under NonCubeElement::faceTextures[2]
// (North) only when the rotation actually still leaves it facing north;
// otherwise the texture moves to whichever face index it's rotated onto.
// No-op (skipped entirely by the caller) when both are 0, which is by far
// the common case (most non-cube blocks — slabs, snow layers, ... — don't
// rotate at all).
void ApplyElementRotation(NonCubeElement& element, int xDeg, int yDeg) {
    glm::vec3 c0 = RotatePoint(element.from, xDeg, yDeg);
    glm::vec3 c1 = RotatePoint(element.to, xDeg, yDeg);
    element.from = glm::min(c0, c1);
    element.to = glm::max(c0, c1);

    std::array<std::string, 6> oldTextures = element.faceTextures;
    std::array<glm::vec4, 6> oldUVs = element.faceUVs;
    element.faceTextures = {};
    element.faceUVs = {};
    for (int f = 0; f < 6; f++) {
        if (oldTextures[static_cast<size_t>(f)].empty()) continue;
        glm::vec3 newNormal = RotateDirection(FACE_NORMALS[static_cast<size_t>(f)], xDeg, yDeg);
        int newFace = FaceIndexFromNormal(newNormal);
        if (newFace < 0) continue;
        element.faceTextures[static_cast<size_t>(newFace)] = oldTextures[static_cast<size_t>(f)];
        element.faceUVs[static_cast<size_t>(newFace)] = oldUVs[static_cast<size_t>(f)];
    }
}

uint16_t InternVisual(const FaceNames& faces) {
    auto it = g_visualIntern.find(faces);
    if (it != g_visualIntern.end()) return it->second;

    uint16_t id = static_cast<uint16_t>(g_visualFaceNames.size());
    g_visualFaceNames.push_back(faces);
    g_visualIntern.emplace(faces, id);
    return id;
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

// True for real fences/walls — every vanilla fence/wall name ends in
// "_fence"/"_wall" (fence gates end in "_fence_gate", excluded by suffix
// mismatch). Other multipart blockstates (redstone dust, panes, ...) stay excluded.
bool IsFenceOrWallName(const std::string& name) {
    auto endsWith = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return endsWith(name, "_fence") || endsWith(name, "_wall");
}

// Vanilla fluid rendering is code-driven, not model-JSON-driven (water.json's
// model resolves to zero elements) — see ResolveWaterVisual.
bool IsWaterName(const std::string& name) {
    return name == "water";
}

// Resolves one state id to a visual id, or 0 if the variant/model/texture
// chain doesn't fully resolve to a renderable full cube. Any unexpected
// shape in the resource-pack JSON (a "texture" value that isn't a string,
// etc.) is treated the same as "doesn't resolve" rather than thrown —
// callers must never have a single odd block's data take down Init() for
// every other block.
uint16_t ResolveStateVisual(const BlockDef& def, const json& blockstateJson, int32_t stateId) {
    try {
        // Known-translucent full cubes (glass, slime, honey, ice, ...) are
        // deliberately excluded here even though they'd otherwise resolve
        // fine — they need ResolveStateNonCubeVisual's alpha-blended,
        // no-backface-cull path below instead, not the opaque one. Water's
        // model has no elements at all (see ResolveWaterVisual) so it would
        // fall through to 0 here regardless; excluded explicitly anyway to
        // say so, rather than relying on that being true incidentally.
        if (IsTransparentCubeName(def.name) || IsWaterName(def.name)) return 0;

        DecodedProps props = DecodeProps(def.states, stateId - def.minStateId);

        std::string modelId;
        int xDeg, yDeg; // Unused here — a full opaque cube's face textures don't depend on the variant's rotation (a pre-existing simplification, unrelated to this pass).
        if (!FindVariantModel(blockstateJson, props, modelId, xDeg, yDeg)) return 0;

        const ResolvedModel& model = ResolveModel(modelId);
        if (!model.hasElements) return 0;
        const json* cubeElement = FindOpaqueCubeElement(model.elements);
        if (!cubeElement) return 0;

        static const char* dirs[6] = { "up", "down", "north", "south", "east", "west" }; // ChunkMesher's 0..5 order.
        const json& faces = (*cubeElement)["faces"];

        FaceNames faceNames;
        for (int f = 0; f < 6; f++) {
            const json& texField = faces[dirs[f]]["texture"];
            if (!texField.is_string()) return 0;
            if (!ResolveTextureRef(model.textures, texField.get<std::string>(), faceNames[static_cast<size_t>(f)])) return 0;
        }

        return InternVisual(faceNames);
    } catch (const std::exception& e) {
        Log::Error("BlockRegistry: failed to resolve " + def.name + " state " + std::to_string(stateId)
            + ": " + e.what());
        return 0;
    }
}

uint16_t InternNonCubeVisual(NonCubeVisual&& visual) {
    uint16_t id = static_cast<uint16_t>(g_nonCubeVisualTable.size());
    g_nonCubeVisualTable.push_back(std::move(visual));
    return id;
}

// Converts one resolved model's "elements" into NonCubeElements, applying
// the variant/part's x/y rotation (see ApplyElementRotation) if either is
// non-zero, and appends them to outElements. Shared by the single-model
// "variants" path (ResolveStateNonCubeVisual) and the multi-part "multipart"
// path (ResolveMultipartNonCubeVisual) below, which builds one visual out of
// several models' elements unioned together (a fence/wall's post plus
// whichever arm models matched this state's connections).
void AppendModelElements(const ResolvedModel& model, int xDeg, int yDeg, std::vector<NonCubeElement>& outElements) {
    static const char* dirs[6] = { "up", "down", "north", "south", "east", "west" }; // ChunkMesher's 0..5 order.

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

                    // Defaults to the full texture when this face doesn't
                    // specify its own "uv" — every vanilla template this
                    // pass actually sees (slabs, snow layers, torches, fence/
                    // wall posts and arms, ...) does specify one, but this
                    // keeps a missing one from rendering as nothing rather
                    // than guessing wrong.
                    glm::vec4 uv(0.0f, 0.0f, 16.0f, 16.0f);
                    const json& faceJson = faces[dirs[f]];
                    if (faceJson.contains("uv") && faceJson["uv"].is_array() && faceJson["uv"].size() == 4) {
                        uv = glm::vec4(
                            faceJson["uv"][0].get<double>(), faceJson["uv"][1].get<double>(),
                            faceJson["uv"][2].get<double>(), faceJson["uv"][3].get<double>());
                    }
                    element.faceUVs[static_cast<size_t>(f)] = uv;
                }
            }
        }

        // Stairs (and anything else a variant/part rotates — see
        // FindVariantModel) bake their model file for one fixed orientation
        // ("facing=east" for vanilla stairs, "north" for a fence/wall arm);
        // the variant/part's x/y turn that into the other facings. Skipped
        // entirely when both are 0 (the common case) rather than running an
        // identity transform through float math for no reason.
        if (xDeg != 0 || yDeg != 0) ApplyElementRotation(element, xDeg, yDeg);

        outElements.push_back(std::move(element));
    }
}

// Builds water's flat-topped box for one state id, from vanilla's "level"
// property (0-15: bit 3 is a falling flag, the low 3 bits are a 0(source)-7
// (thinnest flowing) height stage — see blocks.json's water entry). Height
// ratio (8-stage)/9 matches FluidState's own getOwnHeight; source and
// falling both render full cube height rather than FluidRenderer's real ~8/9
// source fudge factor and per-corner falling logic — a deliberate
// simplification per this task's own "flat lowered top per level" allowance,
// not an attempt at vanilla's sloped-corner interpolation. All six faces
// reuse the single static "water_still" texture (TextureManager only keeps
// frame 0 of an animated PNG — see its own LoadResourcePack comment — so
// there's no flowing-direction texture to pick between anyway) at the full
// 0,0,16,16 UV rect; NonCubicMesher stretches that rect to whatever height
// this element ends up with.
uint16_t ResolveWaterVisual(const BlockDef& def, int32_t stateId) {
    DecodedProps props = DecodeProps(def.states, stateId - def.minStateId);
    auto levelIt = props.find("level");
    int level = (levelIt != props.end()) ? std::atoi(levelIt->second.c_str()) : 0;
    int stage = level & 7;
    bool falling = level >= 8;
    float topY = (falling || stage == 0) ? 16.0f : (8.0f - static_cast<float>(stage)) / 9.0f * 16.0f;

    NonCubeElement element;
    element.from = glm::vec3(0.0f, 0.0f, 0.0f);
    element.to = glm::vec3(16.0f, topY, 16.0f);
    for (int f = 0; f < 6; f++) {
        element.faceTextures[static_cast<size_t>(f)] = "water_still";
        element.faceUVs[static_cast<size_t>(f)] = glm::vec4(0.0f, 0.0f, 16.0f, 16.0f);
    }

    NonCubeVisual visual;
    visual.shape = NonCubeShape::Liquid;
    visual.elements.push_back(std::move(element));
    return InternNonCubeVisual(std::move(visual));
}

// Resolves one state id to a non-cube visual, for any state that
// ResolveStateVisual above couldn't place on the opaque full-cube path.
// Shares FindVariantModel/ResolveModel with the opaque path so both read the
// exact same resolved textures/elements — only the classification differs.
// Water is intercepted before any of that generic parsing — see
// ResolveWaterVisual's own comment for why its model file can't supply it.
uint16_t ResolveStateNonCubeVisual(const BlockDef& def, const json& blockstateJson, int32_t stateId) {
    try {
        if (IsWaterName(def.name)) return ResolveWaterVisual(def, stateId);

        DecodedProps props = DecodeProps(def.states, stateId - def.minStateId);

        std::string modelId;
        int xDeg, yDeg;
        if (!FindVariantModel(blockstateJson, props, modelId, xDeg, yDeg)) return 0;

        const ResolvedModel& model = ResolveModel(modelId);
        if (!model.hasElements) return 0;

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
        AppendModelElements(model, xDeg, yDeg, visual.elements);

        if (visual.elements.empty()) return 0;
        return InternNonCubeVisual(std::move(visual));
    } catch (const std::exception& e) {
        Log::Error("[ERROR] BlockRegistry: failed to resolve non-cube visual for " + def.name
            + " state " + std::to_string(stateId) + ": " + e.what());
        return 0;
    }
}

// Resolves one state id of a "multipart" blockstate to a non-cube visual by
// unioning the elements of every part whose "when" matches this state's
// decoded properties — the same evaluation real Minecraft's own client does.
// Scoped to fences/walls only this pass (see IsFenceOrWallName); every other
// multipart block (redstone dust, tripwire, vines, glass panes, iron bars,
// ...) is still excluded, same as before this function existed.
//
// Deliberately does NOT peek at neighbor blocks to decide connectivity —
// minecraft-data's blocks.json shows oak_fence's states already include
// real north/east/south/west properties (cobblestone_wall's are a
// none/low/tall enum plus "up"), meaning the server has already computed
// and baked the correct connectivity into the very state id it sends over
// the wire (the actual neighbor-aware computation is server-authoritative
// world state, not something the renderer re-derives). Reading it back out
// here is simpler and can't drift from what the server actually decided,
// unlike an independently hand-rolled connection predicate.
uint16_t ResolveMultipartNonCubeVisual(const BlockDef& def, const json& blockstateJson, int32_t stateId) {
    try {
        DecodedProps props = DecodeProps(def.states, stateId - def.minStateId);

        NonCubeVisual visual;
        visual.shape = NonCubeShape::Partial;

        for (const json& part : blockstateJson["multipart"]) {
            if (!part.contains("apply")) continue;

            if (part.contains("when")) {
                const json& when = part["when"];
                // "OR"-style conditions never appear in any real fence/wall
                // blockstate (every "when" here is a flat property match —
                // verified against oak_fence.json/cobblestone_wall.json) —
                // skip rather than mis-render if one ever shows up.
                if (!when.is_object() || when.contains("OR")) continue;

                DecodedProps whenProps;
                for (const auto& [key, value] : when.items()) {
                    whenProps[key] = value.is_string() ? value.get<std::string>() : value.dump();
                }
                if (!VariantKeyMatches(whenProps, props)) continue;
            }

            const json& applyVal = part["apply"];
            const json& chosen = applyVal.is_array() ? applyVal[0] : applyVal;
            if (!chosen.contains("model") || !chosen["model"].is_string()) continue;

            const ResolvedModel& model = ResolveModel(chosen["model"].get<std::string>());
            if (!model.hasElements) continue;

            int xDeg = 0, yDeg = 0;
            if (chosen.contains("x") && chosen["x"].is_number_integer()) xDeg = chosen["x"].get<int>();
            if (chosen.contains("y") && chosen["y"].is_number_integer()) yDeg = chosen["y"].get<int>();

            AppendModelElements(model, xDeg, yDeg, visual.elements);
        }

        if (visual.elements.empty()) return 0;
        return InternNonCubeVisual(std::move(visual));
    } catch (const std::exception& e) {
        Log::Error("BlockRegistry: failed to resolve multipart visual for " + def.name
            + " state " + std::to_string(stateId) + ": " + e.what());
        return 0;
    }
}

// The real body of Init(), split out so the public entry point below can
// wrap it in one last catch-all — nothing in here should be able to bring
// the whole app down over a single bad block/file; anything that does is a
// bug in this function, not a reason to crash the client.
// Loads blockCollisionShapes.json into g_collisionShapes (by shape id) and
// g_stateIdToCollisionShape (by protocol state id). Format: "shapes" maps a
// shape id to a list of [minX,minY,minZ,maxX,maxY,maxZ] boxes in 0..1 block
// units; "blocks" maps a block name to either one shape id (every state) or
// an array indexed by (stateId - minStateId). Leaves both tables empty on any
// failure, which makes MapStateIdCollisionShape report UNKNOWN and every
// block fall back to "opaque full cube or nothing" — degraded, not broken.
void LoadCollisionShapes(const std::vector<BlockDef>& blockDefs, int32_t maxStateId) {
    std::ifstream file(COLLISION_SHAPES_JSON_PATH);
    if (!file) {
        Log::Error("BlockRegistry: couldn't open " + COLLISION_SHAPES_JSON_PATH
            + " — collision falls back to full cubes only (shulker boxes, chests, slabs, ... won't be solid).");
        return;
    }

    json data;
    try {
        file >> data;
    } catch (const std::exception& e) {
        Log::Error(std::string("[ERROR] BlockRegistry: failed to parse blockCollisionShapes.json: ") + e.what());
        return;
    }
    if (!data.contains("shapes") || !data["shapes"].is_object() || !data.contains("blocks") || !data["blocks"].is_object()) {
        Log::Error("BlockRegistry: blockCollisionShapes.json is missing \"shapes\"/\"blocks\".");
        return;
    }

    std::vector<CollisionBoxes> shapes;
    int truncatedShapes = 0;
    for (auto it = data["shapes"].begin(); it != data["shapes"].end(); ++it) {
        int shapeId = std::stoi(it.key());
        if (shapeId < 0 || shapeId >= Block::UNKNOWN_COLLISION_SHAPE) continue;
        if (static_cast<size_t>(shapeId) >= shapes.size()) shapes.resize(static_cast<size_t>(shapeId) + 1);

        CollisionBoxes& boxes = shapes[static_cast<size_t>(shapeId)];
        for (const json& b : it.value()) {
            if (!b.is_array() || b.size() != 6) continue;
            if (boxes.count >= CollisionBoxes::MAX_BOXES) { truncatedShapes++; break; }
            boxes.boxes[static_cast<size_t>(boxes.count++)] = AABB{
                glm::vec3(b[0].get<float>(), b[1].get<float>(), b[2].get<float>()),
                glm::vec3(b[3].get<float>(), b[4].get<float>(), b[5].get<float>()) };
        }
    }
    if (truncatedShapes > 0) {
        Log::Error("BlockRegistry: " + std::to_string(truncatedShapes) + " collision shape(s) had more than "
            + std::to_string(CollisionBoxes::MAX_BOXES) + " boxes; extra boxes dropped.");
    }

    std::vector<uint16_t> stateToShape(static_cast<size_t>(maxStateId) + 1, 0);
    const json& blocks = data["blocks"];
    int missingBlocks = 0;
    for (const BlockDef& def : blockDefs) {
        auto entry = blocks.find(def.name);
        if (entry == blocks.end()) { missingBlocks++; continue; }

        for (int32_t stateId = def.minStateId; stateId <= def.maxStateId; stateId++) {
            int shapeId = 0;
            if (entry->is_number_integer()) {
                shapeId = entry->get<int>();
            } else if (entry->is_array()) {
                size_t index = static_cast<size_t>(stateId - def.minStateId);
                if (index < entry->size() && (*entry)[index].is_number_integer()) shapeId = (*entry)[index].get<int>();
            }
            if (shapeId < 0 || static_cast<size_t>(shapeId) >= shapes.size()) shapeId = 0;
            stateToShape[static_cast<size_t>(stateId)] = static_cast<uint16_t>(shapeId);
        }
    }
    if (missingBlocks > 0) {
        Log::Error("BlockRegistry: " + std::to_string(missingBlocks)
            + " block(s) have no entry in blockCollisionShapes.json; treating them as non-solid.");
    }

    g_collisionShapes = std::move(shapes);
    g_stateIdToCollisionShape = std::move(stateToShape);
    Log::Info("BlockRegistry: loaded " + std::to_string(g_collisionShapes.size()) + " collision shapes.");
}

void InitImpl() {
    g_stateIdToCollisionShape.clear();
    g_collisionShapes.clear();
    g_visualFaceNames.clear();
    g_visualIntern.clear();
    g_nameToVisual.clear();
    g_stateIdToVisual.clear();
    g_stateIdToNonCubeVisual.clear();
    g_nonCubeVisualTable.clear();
    g_modelCache.clear();
    g_stateIdToBreakTicks.clear();
    g_nameToDefaultState.clear();
    g_maxLoadedStateId = -1;

    g_visualFaceNames.push_back(FaceNames{}); // Visual id 0: nothing to draw.
    g_nonCubeVisualTable.push_back(NonCubeVisual{}); // Non-cube visual id 0: nothing to draw.

    Log::Info("BlockRegistry: reading " + BLOCKS_JSON_PATH
        + " (cwd: " + fs::current_path().string() + ")");

    if (!fs::exists(ASSETS_ROOT)) {
        Log::Error("BlockRegistry: resource-pack assets root not found: " + ASSETS_ROOT
            + " — every block will render as nothing this run.");
    }

    std::ifstream blocksFile(BLOCKS_JSON_PATH);
    if (!blocksFile) {
        Log::Error("BlockRegistry: couldn't open " + BLOCKS_JSON_PATH
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

    Log::Info("BlockRegistry: parsed " + std::to_string(blocksJson.size())
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
            def.hardness = b.value("hardness", 0.0f);
            def.diggable = b.value("diggable", true);
            def.requiresTool = b.contains("harvestTools") && b["harvestTools"].is_object() && !b["harvestTools"].empty();

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
            g_nameToDefaultState[def.name] = def.defaultState;
            blockDefs.push_back(std::move(def));
        } catch (const std::exception& e) {
            Log::Error(std::string("[ERROR] BlockRegistry: skipping a malformed block entry in blocks.json: ") + e.what());
        }
    }

    g_stateIdToVisual.assign(static_cast<size_t>(maxStateId) + 1, 0);
    g_stateIdToNonCubeVisual.assign(static_cast<size_t>(maxStateId) + 1, 0);
    g_maxLoadedStateId = maxStateId;

    // Independent of the visual resolution below — collision must not depend
    // on whether a block has drawable model geometry (see GetCollisionBoxes).
    try {
        LoadCollisionShapes(blockDefs, maxStateId);
    } catch (const std::exception& e) {
        g_stateIdToCollisionShape.clear();
        g_collisionShapes.clear();
        Log::Error(std::string("[ERROR] BlockRegistry: failed loading collision shapes: ") + e.what());
    }

    // See MapStateIdBreakTicks — a per-state approximation of vanilla's
    // hand-mining time from blocks.json's own hardness/diggable/harvestTools
    // fields, independent of the visual-resolution loop below.
    g_stateIdToBreakTicks.assign(static_cast<size_t>(maxStateId) + 1, -1);
    for (const BlockDef& def : blockDefs) {
        int32_t ticks;
        if (!def.diggable || def.hardness < 0.0f) {
            ticks = -1; // Unbreakable (bedrock, ...).
        } else if (def.hardness == 0.0f) {
            ticks = 0; // Instant, same as creative — flowers, torches, redstone dust, ...
        } else {
            int divisor = def.requiresTool ? 100 : 30; // See MapStateIdBreakTicks' own comment.
            ticks = std::max(1, static_cast<int32_t>(std::ceil(def.hardness * static_cast<float>(divisor))));
        }
        for (int32_t stateId = def.minStateId; stateId <= def.maxStateId; stateId++) {
            if (stateId < 0 || static_cast<size_t>(stateId) >= g_stateIdToBreakTicks.size()) continue;
            g_stateIdToBreakTicks[static_cast<size_t>(stateId)] = ticks;
        }
    }

    int resolvedBlocks = 0, excludedBlocks = 0, nonCubeResolvedBlocks = 0;

    for (const BlockDef& def : blockDefs) {
        try {
            fs::path bsPath = fs::path(ASSETS_ROOT) / "blockstates" / (def.name + ".json");
            std::ifstream bsFile(bsPath);

            json bsJson;
            bool hasVariants = false;
            bool hasMultipart = false;
            if (bsFile) {
                try {
                    bsFile >> bsJson;
                    hasVariants = bsJson.contains("variants") && bsJson["variants"].is_object();
                    hasMultipart = !hasVariants && bsJson.contains("multipart") && bsJson["multipart"].is_array();
                } catch (const std::exception&) {
                    hasVariants = false;
                    hasMultipart = false;
                }
            }

            // Fences/walls are the one "multipart" shape resolved this pass
            // (see IsFenceOrWallName/ResolveMultipartNonCubeVisual) — every
            // other multipart block still falls through to the exclusion
            // below, same as before.
            bool isFenceOrWall = hasMultipart && IsFenceOrWallName(def.name);

            if (!hasVariants && !isFenceOrWall) {
                // No blockstate file (fluids, cave_air/void_air, ...), or an
                // unhandled "multipart" blockstate (redstone dust, tripwire,
                // vines, glass panes, iron bars, ...) — non-cube by
                // construction. Whole range stays visual 0.
                excludedBlocks++;
                continue;
            }

            bool anyResolved = false;
            bool anyNonCubeResolved = false;
            for (int32_t stateId = def.minStateId; stateId <= def.maxStateId; stateId++) {
                uint16_t visual = 0;
                if (hasVariants) {
                    visual = ResolveStateVisual(def, bsJson, stateId);
                    g_stateIdToVisual[static_cast<size_t>(stateId)] = visual;
                }

                if (visual == 0) {
                    uint16_t nonCubeVisual = isFenceOrWall
                        ? ResolveMultipartNonCubeVisual(def, bsJson, stateId)
                        : ResolveStateNonCubeVisual(def, bsJson, stateId);
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
            Log::Error("BlockRegistry: skipping block \"" + def.name + "\": " + e.what());
            excludedBlocks++;
        }
    }

    Log::Info("BlockRegistry: loaded minecraft-data " + std::string(MC_DATA_VERSION) + " — "
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
            Log::Info("Block state id " + std::to_string(stateId)
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

uint16_t MapStateIdCollisionShape(int32_t stateId) {
    if (stateId >= 0 && static_cast<size_t>(stateId) < g_stateIdToCollisionShape.size()) {
        return g_stateIdToCollisionShape[static_cast<size_t>(stateId)];
    }
    return Block::UNKNOWN_COLLISION_SHAPE;
}

int32_t MapStateIdBreakTicks(int32_t stateId) {
    if (stateId >= 0 && static_cast<size_t>(stateId) < g_stateIdToBreakTicks.size()) {
        return g_stateIdToBreakTicks[static_cast<size_t>(stateId)];
    }
    return -1; // Out-of-range id — treat as unbreakable rather than guessing instant.
}

Block ResolveStateId(int32_t stateId) {
    Block block;
    block.visualId = MapStateId(stateId);
    if (block.visualId == 0) block.nonCubeVisualId = MapStateIdNonCube(stateId);
    block.collisionShapeId = MapStateIdCollisionShape(stateId);
    block.breakTicks = MapStateIdBreakTicks(stateId);
    return block;
}

int32_t DefaultStateIdForName(const std::string& blockName) {
    auto it = g_nameToDefaultState.find(blockName);
    return it != g_nameToDefaultState.end() ? it->second : -1;
}

CollisionBoxes GetCollisionBoxes(const Block& block) {
    if (block.collisionShapeId != Block::UNKNOWN_COLLISION_SHAPE) {
        if (block.collisionShapeId < g_collisionShapes.size()) return g_collisionShapes[block.collisionShapeId];
        return CollisionBoxes{};
    }

    // Fallback — see Block::UNKNOWN_COLLISION_SHAPE.
    CollisionBoxes result;
    if (block.visualId != 0) {
        result.boxes[0] = AABB{ glm::vec3(0.0f), glm::vec3(1.0f) };
        result.count = 1;
    }
    return result;
}

} // namespace Volcano::BlockRegistry
