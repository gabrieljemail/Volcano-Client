#pragma once
#ifndef VOLCANO_MODEL_LOADER_H
#define VOLCANO_MODEL_LOADER_H

#include <array>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include "../../Logger.hpp"
#include "../../resources/ResourceManager.hpp"

namespace Volcano::ModelLoader {

// Resource-pack JSON models (models/block/*, models/item/*) with their
// parent chains already merged: textures, elements and display transforms.
// Used by ItemModel; BlockRegistry still has its own copy of this logic for
// terrain. Not thread-safe (render thread only).

constexpr const char* MODEL_ROOT = "assets/minecraft/models/"; // Logical ResourceManager path.

// Same face order as ChunkMesher/BlockRegistry: 0=Up, 1=Down, 2=North, 3=South, 4=East, 5=West.
constexpr int FACE_COUNT = 6;

struct DisplayTransform
{
    glm::vec3 rotation{0.0f};    // Degrees, applied X then Y then Z.
    glm::vec3 translation{0.0f}; // 1/16 block units.
    glm::vec3 scale{1.0f};
};

struct Face
{
    bool present = false;
    std::string texture; // Raw reference: "#all", "minecraft:block/stone", ...
    glm::vec4 uv{0.0f};  // u1, v1, u2, v2 in 0..16 (valid when hasUV).
    bool hasUV = false;
    int rotation = 0;    // 0/90/180/270.
    int tintIndex = -1;
};

struct Element
{
    glm::vec3 from{0.0f};
    glm::vec3 to{16.0f};
    std::array<Face, FACE_COUNT> faces;
    bool hasRotation = false;
    glm::vec3 rotationOrigin{8.0f};
    int rotationAxis = 1; // 0=x, 1=y, 2=z.
    float rotationAngle = 0.0f;
    bool rescale = false;
};

struct Model
{
    std::string root; // "builtin/generated", "builtin/entity", ... or "" for a plain element model.
    std::unordered_map<std::string, std::string> textures;
    std::vector<Element> elements;
    std::unordered_map<std::string, DisplayTransform> display;
};

namespace detail
{
    inline std::string StripNamespace(std::string id)
    {
        constexpr const char* prefix = "minecraft:";
        if (id.rfind(prefix, 0) == 0) id.erase(0, 10);
        return id;
    }

    inline glm::vec3 ReadVec3(const nlohmann::json& j, glm::vec3 fallback)
    {
        if (!j.is_array() || j.size() < 3) return fallback;
        return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    }

    inline int FaceIndex(const std::string& name)
    {
        if (name == "up") return 0;
        if (name == "down") return 1;
        if (name == "north") return 2;
        if (name == "south") return 3;
        if (name == "east") return 4;
        if (name == "west") return 5;
        return -1;
    }

    inline std::unordered_map<std::string, std::unique_ptr<Model>>& Cache()
    {
        static std::unordered_map<std::string, std::unique_ptr<Model>> cache;
        return cache;
    }

    inline std::unique_ptr<Model> Parse(const std::string& id, int depth);
}

// Loads and merges a model by id ("minecraft:block/anvil", "item/stone_sword").
// nullptr if the file is missing or malformed. Results are cached.
inline const Model* Load(const std::string& modelId)
{
    std::string id = detail::StripNamespace(modelId);
    auto& cache = detail::Cache();
    if (auto it = cache.find(id); it != cache.end()) return it->second.get();

    std::unique_ptr<Model> model = detail::Parse(id, 0);
    const Model* result = model.get();
    cache[id] = std::move(model);
    return result;
}

// Drops every cached model (call after ResourceManager::Reload).
inline void ClearCache()
{
    detail::Cache().clear();
}

// Follows "#name" texture references through the merged textures map and
// returns a path like "block/stone" (namespace stripped), or "" if it can't
// be resolved.
inline std::string ResolveTexture(const Model& model, std::string reference)
{
    for (int depth = 0; depth < 16; depth++)
    {
        if (reference.empty()) return {};
        if (reference[0] != '#') return detail::StripNamespace(reference);

        auto it = model.textures.find(reference.substr(1));
        if (it == model.textures.end()) return {};
        reference = it->second;
    }
    return {};
}

namespace detail
{
    inline std::unique_ptr<Model> Parse(const std::string& id, int depth)
    {
        if (depth > 16) return nullptr;

        auto bytes = ResourceManager::Instance().ReadBytes(std::string(MODEL_ROOT) + id + ".json");
        if (!bytes) return nullptr;

        nlohmann::json j;
        try
        {
            j = nlohmann::json::parse(bytes->begin(), bytes->end());
        }
        catch (const std::exception& e)
        {
            Log::Error("ModelLoader: failed to parse " + id + ": " + e.what());
            return nullptr;
        }

        auto model = std::make_unique<Model>();
        try
        {
            if (j.contains("parent") && j["parent"].is_string())
            {
                std::string parent = StripNamespace(j["parent"].get<std::string>());
                if (parent.rfind("builtin/", 0) == 0)
                {
                    model->root = parent;
                }
                else
                {
                    std::unique_ptr<Model> parentModel = Parse(parent, depth + 1);
                    if (parentModel) *model = *parentModel;
                }
            }

            if (j.contains("textures") && j["textures"].is_object())
            {
                for (auto& [key, value] : j["textures"].items())
                {
                    if (value.is_string()) model->textures[key] = value.get<std::string>();
                }
            }

            if (j.contains("display") && j["display"].is_object())
            {
                for (auto& [key, value] : j["display"].items())
                {
                    DisplayTransform transform;
                    transform.rotation = ReadVec3(value.value("rotation", nlohmann::json()), glm::vec3(0.0f));
                    transform.translation = ReadVec3(value.value("translation", nlohmann::json()), glm::vec3(0.0f));
                    transform.scale = ReadVec3(value.value("scale", nlohmann::json()), glm::vec3(1.0f));
                    model->display[key] = transform;
                }
            }

            if (j.contains("elements") && j["elements"].is_array())
            {
                model->elements.clear();
                for (const auto& e : j["elements"])
                {
                    Element element;
                    element.from = ReadVec3(e.value("from", nlohmann::json()), glm::vec3(0.0f));
                    element.to = ReadVec3(e.value("to", nlohmann::json()), glm::vec3(16.0f));

                    if (e.contains("rotation") && e["rotation"].is_object())
                    {
                        const auto& r = e["rotation"];
                        element.hasRotation = true;
                        element.rotationOrigin = ReadVec3(r.value("origin", nlohmann::json()), glm::vec3(8.0f));
                        std::string axis = r.value("axis", "y");
                        element.rotationAxis = axis == "x" ? 0 : (axis == "z" ? 2 : 1);
                        element.rotationAngle = r.value("angle", 0.0f);
                        element.rescale = r.value("rescale", false);
                    }

                    if (e.contains("faces") && e["faces"].is_object())
                    {
                        for (auto& [name, f] : e["faces"].items())
                        {
                            int index = FaceIndex(name);
                            if (index < 0) continue;

                            Face& face = element.faces[static_cast<size_t>(index)];
                            face.present = true;
                            face.texture = f.value("texture", "");
                            face.rotation = f.value("rotation", 0);
                            face.tintIndex = f.value("tintindex", -1);
                            if (f.contains("uv") && f["uv"].is_array() && f["uv"].size() >= 4)
                            {
                                face.hasUV = true;
                                face.uv = glm::vec4(f["uv"][0].get<float>(), f["uv"][1].get<float>(),
                                                    f["uv"][2].get<float>(), f["uv"][3].get<float>());
                            }
                        }
                    }
                    model->elements.push_back(element);
                }
            }
        }
        catch (const std::exception& e)
        {
            Log::Error("ModelLoader: bad model " + id + ": " + e.what());
            return nullptr;
        }
        return model;
    }
}

} // namespace Volcano::ModelLoader

#endif
