#include "ItemModel.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>
#include "../TextureManager.hpp"
#include "../../Logger.hpp"
#include "../../resources/ImageLoad.hpp"

namespace Volcano {

namespace {

// Logical ResourceManager paths.
constexpr const char* ITEM_DEFINITION_ROOT = "assets/minecraft/items/";
constexpr const char* TEXTURE_ROOT = "assets/minecraft/textures/";
constexpr float EXTRUDE_THICKNESS = 1.0f / 16.0f;
constexpr int SPRITE_SIZE = 16;

// An item definition node ("minecraft:model", "minecraft:select", ...)
// reduced to the one model id it would use with no special context: the
// fallback case, the "off" branch, or the first entry.
std::string ModelIdFromDefinition(const nlohmann::json& node, int depth = 0)
{
    if (depth > 8 || !node.is_object()) return {};

    std::string type = node.value("type", "");
    if (type.rfind("minecraft:", 0) == 0) type.erase(0, 10);

    if (type == "model") return node.value("model", "");
    if (type == "special") return node.value("base", "");
    if (type == "condition") return node.contains("on_false") ? ModelIdFromDefinition(node["on_false"], depth + 1) : "";
    if (type == "composite")
    {
        if (node.contains("models") && node["models"].is_array() && !node["models"].empty())
        {
            return ModelIdFromDefinition(node["models"][0], depth + 1);
        }
        return {};
    }
    if (type == "select" || type == "range_dispatch")
    {
        if (node.contains("fallback")) return ModelIdFromDefinition(node["fallback"], depth + 1);
        const char* listKey = type == "select" ? "cases" : "entries";
        if (node.contains(listKey) && node[listKey].is_array() && !node[listKey].empty() && node[listKey][0].contains("model"))
        {
            return ModelIdFromDefinition(node[listKey][0]["model"], depth + 1);
        }
    }
    return {};
}

std::string FindModelId(const std::string& itemName)
{
    auto bytes = ResourceManager::Instance().ReadBytes(std::string(ITEM_DEFINITION_ROOT) + itemName + ".json");
    if (bytes)
    {
        try
        {
            nlohmann::json j = nlohmann::json::parse(bytes->begin(), bytes->end());
            if (j.contains("model"))
            {
                std::string id = ModelIdFromDefinition(j["model"]);
                if (!id.empty()) return id;
            }
        }
        catch (const std::exception& e)
        {
            Log::Error("ItemModel: bad item definition for " + itemName + ": " + e.what());
        }
    }
    // No (usable) definition: the conventional model locations.
    if (ModelLoader::Load("item/" + itemName) != nullptr) return "item/" + itemName;
    return "block/" + itemName;
}

// Appends one quad (corners in BL, BR, TR, TL order, CCW from outside) with
// the given per-corner UVs.
void AddQuad(std::vector<ItemModelVertex>& vertices, std::vector<uint32_t>& indices,
    const std::array<glm::vec3, 4>& corners, const std::array<glm::vec2, 4>& uvs, glm::vec3 normal, float layer)
{
    uint32_t base = static_cast<uint32_t>(vertices.size());
    for (size_t i = 0; i < 4; i++) vertices.push_back({ corners[i], normal, uvs[i], layer });
    indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
}

// Texture layer for a resolved texture path like "block/stone" (TextureManager
// keys layers by bare file name), or -1 if the pack has no such texture.
int LayerFor(const TextureManager& textures, const std::string& path)
{
    std::string stem = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
    if (!textures.HasLayer(stem)) return -1;
    return static_cast<int>(textures.GetLayerIndex(stem));
}

struct FaceFrame { glm::vec3 normal, right, up; };

// Same frames as EntityRenderer's block mesh: right x up == normal, viewed
// from outside. Order: Up, Down, North, South, East, West.
constexpr std::array<FaceFrame, ModelLoader::FACE_COUNT> FACE_FRAMES = {{
    { glm::vec3( 0, 1, 0), glm::vec3( 1, 0, 0), glm::vec3( 0, 0,-1) },
    { glm::vec3( 0,-1, 0), glm::vec3( 1, 0, 0), glm::vec3( 0, 0, 1) },
    { glm::vec3( 0, 0,-1), glm::vec3(-1, 0, 0), glm::vec3( 0, 1, 0) },
    { glm::vec3( 0, 0, 1), glm::vec3( 1, 0, 0), glm::vec3( 0, 1, 0) },
    { glm::vec3( 1, 0, 0), glm::vec3( 0, 0,-1), glm::vec3( 0, 1, 0) },
    { glm::vec3(-1, 0, 0), glm::vec3( 0, 0, 1), glm::vec3( 0, 1, 0) },
}};

// Vanilla's default UV for a face with no explicit "uv" (0..16 space).
glm::vec4 DefaultFaceUV(int face, glm::vec3 from, glm::vec3 to)
{
    switch (face)
    {
        case 0: return { from.x, from.z, to.x, to.z };                       // Up
        case 1: return { from.x, 16.0f - to.z, to.x, 16.0f - from.z };       // Down
        case 2: return { 16.0f - to.x, 16.0f - to.y, 16.0f - from.x, 16.0f - from.y }; // North
        case 3: return { from.x, 16.0f - to.y, to.x, 16.0f - from.y };       // South
        case 4: return { 16.0f - to.z, 16.0f - to.y, 16.0f - from.z, 16.0f - from.y };  // East
        default: return { from.z, 16.0f - to.y, to.z, 16.0f - from.y };      // West
    }
}

} // namespace

// Builds the mesh; a friend of ItemModel so the members stay private.
class ItemModelBuilder {
public:
    static std::unique_ptr<ItemModel> Build(const std::string& itemName, const TextureManager& textures)
    {
        const ModelLoader::Model* model = ModelLoader::Load(FindModelId(itemName));
        if (model == nullptr) return nullptr;

        auto item = std::make_unique<ItemModel>();
        item->display = model->display;

        if (model->root == "builtin/generated")
        {
            item->type = ItemModelType::ITEM;
            BuildExtruded(*model, textures, *item);
        }
        else if (!model->elements.empty())
        {
            bool fullCube = model->elements.size() == 1 && model->elements[0].from == glm::vec3(0.0f)
                && model->elements[0].to == glm::vec3(16.0f) && !model->elements[0].hasRotation;
            item->type = fullCube ? ItemModelType::BLOCK : ItemModelType::CUSTOM;
            BuildElements(*model, textures, *item);
        }

        if (item->indices.empty()) return nullptr;
        return item;
    }

private:
    // Front/back plus one side quad per exposed edge pixel, so the sprite
    // has thickness - vanilla's look for flat items.
    static void BuildExtruded(const ModelLoader::Model& model, const TextureManager& textures, ItemModel& item)
    {
        const float z0 = 0.5f - EXTRUDE_THICKNESS * 0.5f;
        const float z1 = 0.5f + EXTRUDE_THICKNESS * 0.5f;

        for (int layerIndex = 0; layerIndex < 5; layerIndex++)
        {
            auto textureRef = model.textures.find("layer" + std::to_string(layerIndex));
            if (textureRef == model.textures.end()) break;

            std::string path = ModelLoader::ResolveTexture(model, textureRef->second);
            int layer = path.empty() ? -1 : LayerFor(textures, path);
            if (layer < 0) continue;
            float layerValue = static_cast<float>(layer);

            // Later layers (overlays) sit a hair in front/behind to avoid z-fighting.
            float offset = layerIndex * 0.0005f;
            float front = z1 + offset;
            float back = z0 - offset;

            AddQuad(item.vertices, item.indices,
                { glm::vec3(0, 0, front), glm::vec3(1, 0, front), glm::vec3(1, 1, front), glm::vec3(0, 1, front) },
                { glm::vec2(0, 1), glm::vec2(1, 1), glm::vec2(1, 0), glm::vec2(0, 0) }, glm::vec3(0, 0, 1), layerValue);
            AddQuad(item.vertices, item.indices,
                { glm::vec3(1, 0, back), glm::vec3(0, 0, back), glm::vec3(0, 1, back), glm::vec3(1, 1, back) },
                { glm::vec2(1, 1), glm::vec2(0, 1), glm::vec2(0, 0), glm::vec2(1, 0) }, glm::vec3(0, 0, -1), layerValue);

            // Opaque pixels, for the side walls.
            std::string file = std::string(TEXTURE_ROOT) + path + ".png";
            int width = 0, height = 0;
            uint8_t* pixels = LoadImageRGBA(file, width, height);
            if (pixels == nullptr) continue;

            auto opaque = [&](int x, int y)
            {
                if (x < 0 || y < 0 || x >= SPRITE_SIZE || y >= SPRITE_SIZE) return false;
                int sx = x * width / SPRITE_SIZE;
                int sy = y * width / SPRITE_SIZE; // Animated strips are square frames stacked vertically.
                return pixels[(static_cast<size_t>(sy) * width + sx) * 4 + 3] > 0;
            };

            for (int py = 0; py < SPRITE_SIZE; py++)
            {
                for (int px = 0; px < SPRITE_SIZE; px++)
                {
                    if (!opaque(px, py)) continue;

                    float x0 = px / 16.0f, x1 = (px + 1) / 16.0f;
                    float yTop = 1.0f - py / 16.0f, yBottom = 1.0f - (py + 1) / 16.0f;
                    glm::vec2 centerUV((px + 0.5f) / 16.0f, (py + 0.5f) / 16.0f);
                    std::array<glm::vec2, 4> uvs = { centerUV, centerUV, centerUV, centerUV };

                    if (!opaque(px - 1, py)) // -X
                        AddQuad(item.vertices, item.indices,
                            { glm::vec3(x0, yBottom, back), glm::vec3(x0, yBottom, front), glm::vec3(x0, yTop, front), glm::vec3(x0, yTop, back) },
                            uvs, glm::vec3(-1, 0, 0), layerValue);
                    if (!opaque(px + 1, py)) // +X
                        AddQuad(item.vertices, item.indices,
                            { glm::vec3(x1, yBottom, front), glm::vec3(x1, yBottom, back), glm::vec3(x1, yTop, back), glm::vec3(x1, yTop, front) },
                            uvs, glm::vec3(1, 0, 0), layerValue);
                    if (!opaque(px, py - 1)) // +Y
                        AddQuad(item.vertices, item.indices,
                            { glm::vec3(x0, yTop, front), glm::vec3(x1, yTop, front), glm::vec3(x1, yTop, back), glm::vec3(x0, yTop, back) },
                            uvs, glm::vec3(0, 1, 0), layerValue);
                    if (!opaque(px, py + 1)) // -Y
                        AddQuad(item.vertices, item.indices,
                            { glm::vec3(x0, yBottom, back), glm::vec3(x1, yBottom, back), glm::vec3(x1, yBottom, front), glm::vec3(x0, yBottom, front) },
                            uvs, glm::vec3(0, -1, 0), layerValue);
                }
            }
            stbi_image_free(pixels);
        }
    }

    static void BuildElements(const ModelLoader::Model& model, const TextureManager& textures, ItemModel& item)
    {
        for (const ModelLoader::Element& element : model.elements)
        {
            glm::vec3 center = (element.from + element.to) * 0.5f;
            glm::vec3 size = element.to - element.from;

            // Element rotation about its own origin (16-space), applied to
            // positions and normals after the box is laid out.
            glm::mat4 rotation(1.0f);
            if (element.hasRotation && element.rotationAngle != 0.0f)
            {
                glm::vec3 axis(0.0f);
                axis[element.rotationAxis] = 1.0f;
                rotation = glm::translate(glm::mat4(1.0f), element.rotationOrigin)
                    * glm::rotate(glm::mat4(1.0f), glm::radians(element.rotationAngle), axis)
                    * glm::translate(glm::mat4(1.0f), -element.rotationOrigin);
            }

            for (int f = 0; f < ModelLoader::FACE_COUNT; f++)
            {
                const ModelLoader::Face& face = element.faces[static_cast<size_t>(f)];
                if (!face.present) continue;

                std::string path = ModelLoader::ResolveTexture(model, face.texture);
                int layer = path.empty() ? -1 : LayerFor(textures, path);
                if (layer < 0) continue;

                const FaceFrame& frame = FACE_FRAMES[static_cast<size_t>(f)];
                glm::vec3 faceCenter = center + frame.normal * (glm::dot(size, glm::abs(frame.normal)) * 0.5f);
                glm::vec3 halfRight = frame.right * (glm::dot(size, glm::abs(frame.right)) * 0.5f);
                glm::vec3 halfUp = frame.up * (glm::dot(size, glm::abs(frame.up)) * 0.5f);

                std::array<glm::vec3, 4> corners = {
                    faceCenter - halfRight - halfUp,
                    faceCenter + halfRight - halfUp,
                    faceCenter + halfRight + halfUp,
                    faceCenter - halfRight + halfUp,
                };
                for (glm::vec3& corner : corners)
                {
                    corner = glm::vec3(rotation * glm::vec4(corner, 1.0f)) / 16.0f;
                }
                glm::vec3 normal = glm::normalize(glm::vec3(rotation * glm::vec4(frame.normal, 0.0f)));

                glm::vec4 uv = face.hasUV ? face.uv : DefaultFaceUV(f, element.from, element.to);
                float u0 = uv.x / 16.0f, v0 = uv.y / 16.0f, u1 = uv.z / 16.0f, v1 = uv.w / 16.0f;
                std::array<glm::vec2, 4> uvs = { glm::vec2(u0, v1), glm::vec2(u1, v1), glm::vec2(u1, v0), glm::vec2(u0, v0) };

                int steps = ((face.rotation / 90) % 4 + 4) % 4;
                if (steps != 0)
                {
                    std::array<glm::vec2, 4> rotated;
                    for (int i = 0; i < 4; i++) rotated[static_cast<size_t>(i)] = uvs[static_cast<size_t>((i + steps) % 4)];
                    uvs = rotated;
                }

                AddQuad(item.vertices, item.indices, corners, uvs, normal, static_cast<float>(layer));
            }
        }
    }
};

namespace {

std::unordered_map<std::string, std::unique_ptr<ItemModel>>& ModelCache()
{
    static std::unordered_map<std::string, std::unique_ptr<ItemModel>> cache;
    return cache;
}

} // namespace

void ItemModel::ClearCache()
{
    ModelCache().clear();
}

const ItemModel* ItemModel::ForItem(const std::string& itemName, const TextureManager& textures)
{
    auto& cache = ModelCache();
    if (auto it = cache.find(itemName); it != cache.end()) return it->second.get();

    std::unique_ptr<ItemModel> model = ItemModelBuilder::Build(itemName, textures);
    const ItemModel* result = model.get();
    cache[itemName] = std::move(model);
    return result;
}

glm::mat4 ItemModel::DisplayMatrix(const std::string& context) const
{
    // Left-hand contexts fall back to the right hand's entry, and either way
    // are mirrored (ItemTransform.apply's leftHand flag): yaw and roll are
    // negated and the X translation flipped.
    bool leftHand = context.size() > 8 && context.compare(context.size() - 8, 8, "lefthand") == 0;
    ModelLoader::DisplayTransform transform;
    if (auto it = display.find(context); it != display.end())
    {
        transform = it->second;
    }
    else if (leftHand)
    {
        std::string rightContext = context.substr(0, context.size() - 8) + "righthand";
        if (auto right = display.find(rightContext); right != display.end()) transform = right->second;
    }
    if (leftHand)
    {
        transform.rotation.y = -transform.rotation.y;
        transform.rotation.z = -transform.rotation.z;
        transform.translation.x = -transform.translation.x;
    }

    glm::vec3 translation = glm::clamp(transform.translation, glm::vec3(-80.0f), glm::vec3(80.0f)) / 16.0f;
    glm::vec3 scale = glm::clamp(transform.scale, glm::vec3(-4.0f), glm::vec3(4.0f));

    // Quaternion::rotationXYZ is Qx * Qy * Qz, so a vertex is rotated about Z
    // first, then Y, then X.
    glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), glm::radians(transform.rotation.x), glm::vec3(1, 0, 0))
        * glm::rotate(glm::mat4(1.0f), glm::radians(transform.rotation.y), glm::vec3(0, 1, 0))
        * glm::rotate(glm::mat4(1.0f), glm::radians(transform.rotation.z), glm::vec3(0, 0, 1));

    return glm::translate(glm::mat4(1.0f), translation) * rotation * glm::scale(glm::mat4(1.0f), scale)
        * glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f));
}

} // namespace Volcano
