#include "ParticleSprites.hpp"
#include <cstring>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "../../Logger.hpp"
#include "../../resources/ImageLoad.hpp"

namespace Volcano {

namespace {

constexpr const char* DEFINITIONS_DIR = "assets/minecraft/particles";
constexpr const char* TEXTURE_DIR = "assets/minecraft/textures/particle/";
constexpr float VANILLA_PARTICLE_PIXELS = 8.0f; // generic_0.png in the base pack.

} // namespace

bool ParticleSprites::Load()
{
    byType.clear();
    pixels.clear();
    layerCount = 0;
    ResourceManager& resources = ResourceManager::Instance();

    std::unordered_map<std::string, ParticleSprite> placed;
    std::unordered_set<std::string> unusable;

    // A pack's resolution is read off a texture every pack redraws: generic_0 is 8px in vanilla.
    float resolutionScale = 1.0f;
    {
        int width = 0, height = 0;
        if (stbi_uc* probe = LoadImageRGBA(std::string(TEXTURE_DIR) + "generic_0.png", width, height))
        {
            if (width > 0) resolutionScale = VANILLA_PARTICLE_PIXELS / static_cast<float>(width);
            stbi_image_free(probe);
        }
    }

    // Each texture goes in the atlas once, however many types list it.
    auto place = [&](const std::string& texture) -> const ParticleSprite*
    {
        auto found = placed.find(texture);
        if (found != placed.end()) return &found->second;
        if (unusable.count(texture) != 0) return nullptr;

        int width = 0, height = 0;
        stbi_uc* image = LoadImageRGBA(TEXTURE_DIR + texture + ".png", width, height);
        bool fits = image != nullptr && width > 0 && height > 0
            && static_cast<uint32_t>(width) <= LAYER_SIZE && static_cast<uint32_t>(height) <= LAYER_SIZE;
        if (!fits)
        {
            if (image != nullptr) stbi_image_free(image);
            unusable.insert(texture);
            return nullptr;
        }

        ParticleSprite sprite;
        sprite.layer = layerCount++;
        sprite.scale = resolutionScale;
        sprite.u1 = static_cast<float>(width) / static_cast<float>(LAYER_SIZE);
        sprite.v1 = static_cast<float>(height) / static_cast<float>(LAYER_SIZE);
        const size_t layerBytes = static_cast<size_t>(LAYER_SIZE) * LAYER_SIZE * 4;
        pixels.resize(pixels.size() + layerBytes, 0);
        uint8_t* layer = pixels.data() + static_cast<size_t>(sprite.layer) * layerBytes;
        for (int y = 0; y < height; y++)
        {
            std::memcpy(layer + static_cast<size_t>(y) * LAYER_SIZE * 4, image + static_cast<size_t>(y) * width * 4,
                static_cast<size_t>(width) * 4);
        }
        stbi_image_free(image);
        return &placed.emplace(texture, sprite).first->second;
    };

    for (const std::string& path : resources.List(DEFINITIONS_DIR, ".json", false))
    {
        auto bytes = resources.ReadBytes(path);
        if (!bytes) continue;
        nlohmann::json definition = nlohmann::json::parse(bytes->begin(), bytes->end(), nullptr, false);
        if (!definition.is_object() || !definition.contains("textures") || !definition["textures"].is_array()) continue;

        size_t slash = path.find_last_of('/');
        std::string type = path.substr(slash == std::string::npos ? 0 : slash + 1);
        type = type.substr(0, type.size() - std::strlen(".json"));

        std::vector<ParticleSprite> sprites;
        for (const nlohmann::json& entry : definition["textures"])
        {
            if (!entry.is_string()) continue;
            std::string name = entry.get<std::string>();
            size_t colon = name.find(':'); // "minecraft:flame" is the texture "flame".
            if (colon != std::string::npos) name = name.substr(colon + 1);
            if (const ParticleSprite* sprite = place(name)) sprites.push_back(*sprite);
        }
        if (!sprites.empty()) byType[type] = std::move(sprites);
    }

    Log::Info("Particles: " + std::to_string(byType.size()) + " types, " + std::to_string(layerCount) + " textures.");
    return layerCount > 0;
}

const std::vector<ParticleSprite>& ParticleSprites::Of(const std::string& type) const
{
    static const std::vector<ParticleSprite> none;
    auto found = byType.find(type);
    return found != byType.end() ? found->second : none;
}

} // namespace Volcano
