#define NOMINMAX
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <stb_image.h>
#include "D3D11EntityAssets.hpp"
#include "../../../../ApplicationDeps.hpp"
import Volcano;
#include "../../../../Logger.hpp"
#include "../../../../network/SkinFetcher.hpp"
#include "../../../../resources/ImageLoad.hpp"
#include "../../../entity/models/EntityAnimation.hpp"
#include "../../../entity/models/EntityJsonModel.hpp"
#include "../../../terrain/models/BlockRegistry.hpp"

namespace Volcano::Engine::D3D11 {

namespace {

constexpr const char* MOB_MODEL_DIR = "resources/volcano/models/entity/";
constexpr const char* MOB_SKIN_DIR = "assets/minecraft/textures/entity/"; // Logical pack path.
constexpr size_t MAX_PLAYER_SKINS = 96;
constexpr size_t MAX_BLOCK_TEXTURES = 48;
constexpr int BLOCK_TILE_SIZE = 16;

// Texture name for one face of a block, falling back to non-cube visuals
// (slabs, glass, plants) so they at least show something cube-shaped.
std::string BlockFaceTextureName(const Block& block, int face)
{
    if (block.visualId != 0) return BlockRegistry::GetFaceTextureName(block.visualId, face);

    const BlockRegistry::NonCubeVisual& visual = BlockRegistry::GetNonCubeVisual(block.nonCubeVisualId);
    if (!visual.crossTexture.empty()) return visual.crossTexture;
    for (const auto& element : visual.elements)
    {
        if (!element.faceTextures[static_cast<size_t>(face)].empty()) return element.faceTextures[static_cast<size_t>(face)];
    }
    for (const auto& element : visual.elements)
    {
        for (const std::string& name : element.faceTextures)
        {
            if (!name.empty()) return name;
        }
    }
    return {};
}

} // namespace

void D3D11EntityAssets::CreateSampler()
{
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&desc, &sampler)))
    {
        throw std::runtime_error("[ERROR] Failed to create the entity skin sampler.");
    }
}

bool D3D11EntityAssets::CreateSkin(const uint8_t* rgba, int width, int height, Skin& out)
{
    if (rgba == nullptr || width <= 0 || height <= 0) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{ rgba, static_cast<UINT>(width) * 4, 0 };

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, &data, &texture))) return false;
    return SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &out.view));
}

const D3D11EntityAssets::Skin* D3D11EntityAssets::CachedSkin(const std::string& skinPath)
{
    auto found = skinCache.find(skinPath);
    if (found != skinCache.end()) return &found->second;

    int width = 0, height = 0;
    uint8_t* pixels = LoadImageRGBA(MOB_SKIN_DIR + skinPath, width, height);
    if (pixels == nullptr)
    {
        throw std::runtime_error("[ERROR] EntityRenderer: failed to load skin texture: " + skinPath);
    }
    Skin skin;
    bool ok = CreateSkin(pixels, width, height, skin);
    stbi_image_free(pixels);
    if (!ok) throw std::runtime_error("[ERROR] EntityRenderer: failed to upload skin texture: " + skinPath);
    return &skinCache.emplace(skinPath, std::move(skin)).first->second;
}

const D3D11EntityAssets::Mesh* D3D11EntityAssets::CachedMesh(const std::string& modelFile, float scale)
{
    const std::string key = modelFile + "@" + std::to_string(scale);
    auto found = meshCache.find(key);
    if (found != meshCache.end()) return &found->second;

    EntityJsonModel::ModelDef model = EntityJsonModel::Load(MOB_MODEL_DIR + modelFile);
    std::vector<EntityJsonModel::Vertex> vertices;
    std::vector<uint32_t> indices;
    EntityJsonModel::MeshInfo info = EntityJsonModel::BuildMesh(model, vertices, indices, scale, true);

    Mesh mesh;
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.scale = scale;
    mesh.feetHeight = model.feetY / 16.0f * scale;
    mesh.parts = std::move(info.parts);
    mesh.roles = EntityAnimation::FindRoles(mesh.parts);

    D3D11_BUFFER_DESC desc{};
    D3D11_SUBRESOURCE_DATA data{};
    desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(EntityJsonModel::Vertex));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    data.pSysMem = vertices.data();
    if (vertices.empty() || FAILED(device->CreateBuffer(&desc, &data, &mesh.vertexBuffer)))
    {
        throw std::runtime_error("[ERROR] Failed to create a mob vertex buffer: " + modelFile);
    }
    desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = indices.data();
    if (FAILED(device->CreateBuffer(&desc, &data, &mesh.indexBuffer)))
    {
        throw std::runtime_error("[ERROR] Failed to create a mob index buffer: " + modelFile);
    }
    return &meshCache.emplace(key, std::move(mesh)).first->second;
}

void D3D11EntityAssets::Load()
{
    if (!sampler) CreateSampler();

    const std::string path = std::string(MOB_MODEL_DIR) + "mobs.json";
    nlohmann::json manifest;
    try
    {
        std::ifstream file(path);
        if (!file.is_open()) throw std::runtime_error("failed to open " + path);
        file >> manifest;
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(std::string("EntityRenderer: mob manifest unreadable: ") + e.what());
    }

    for (const auto& [name, entry] : manifest.at("mobs").items())
    {
        try
        {
            float scale = entry.value("scale", 1.0f);
            EntityScene::MobModel mob;
            mob.layers.push_back({ CachedMesh(entry.at("model").get<std::string>(), scale),
                CachedSkin(entry.at("skin").get<std::string>()) });
            std::string pitch = entry.value("pitch", std::string());
            mob.pitchMode = pitch == "look" ? 1 : pitch == "projectile" ? -1 : 0;
            if (entry.value("armor", false))
            {
                mob.armorOuter = CachedMesh("armor_outer.json", scale);
                mob.armorInner = CachedMesh("armor_inner.json", scale);
            }
            if (entry.contains("layers"))
            {
                for (const auto& layer : entry.at("layers"))
                {
                    mob.layers.push_back({ CachedMesh(layer.at("model").get<std::string>(), scale),
                        CachedSkin(layer.at("skin").get<std::string>()) });
                }
            }
            mobs[name] = std::move(mob);
        }
        catch (const std::exception& e)
        {
            Log::Error("EntityRenderer: skipping mob '" + name + "': " + e.what());
        }
    }
    Log::Info("D3D11 entities: " + std::to_string(mobs.size()) + " textured mob models loaded.");
}

void D3D11EntityAssets::Release()
{
    mobs.clear();
    glintSkin = Skin{};
    glintSampler.Reset();
    glintTried = false;
    blockTextures.clear();
    blockMesh = Mesh{};
    blockMeshBuilt = false;
    playerSkins.clear();
    armorSkins.clear();
    skinCache.clear();
    meshCache.clear();
    sampler.Reset();
}

void D3D11EntityAssets::UploadFinishedSkins(Application* state)
{
    if (state->skinFetcher == nullptr) return;
    for (SkinImage& image : state->skinFetcher->TakeCompleted())
    {
        if (playerSkins.size() >= MAX_PLAYER_SKINS && playerSkins.find(image.key) == playerSkins.end()) continue;
        PlayerSkin skin;
        if (image.pixels.size() < 64u * 64u * 4u || !CreateSkin(image.pixels.data(), 64, 64, skin.skin))
        {
            Log::Error("EntityRenderer: player skin upload failed: " + image.key);
            continue;
        }
        skin.slim = image.slim;
        playerSkins[image.key] = std::move(skin);
    }
}

const EntityScene::SkinBase* D3D11EntityAssets::ArmorGlintSkin()
{
    if (!glintTried)
    {
        glintTried = true;
        int width = 0, height = 0;
        uint8_t* pixels = LoadImageRGBA("assets/minecraft/textures/misc/enchanted_glint_armor.png", width, height);
        bool ok = false;
        if (pixels != nullptr)
        {
            ok = CreateSkin(pixels, width, height, glintSkin);
            stbi_image_free(pixels);
        }
        if (ok)
        {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
            desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            desc.MaxAnisotropy = 1;
            desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            desc.MaxLOD = D3D11_FLOAT32_MAX;
            ok = SUCCEEDED(device->CreateSamplerState(&desc, &glintSampler));
        }
        if (!ok) glintSkin.view.Reset();
    }
    return glintSkin.view ? &glintSkin : nullptr;
}

const D3D11EntityAssets::Mesh* D3D11EntityAssets::BlockMesh()
{
    if (blockMeshBuilt) return blockMesh.vertexBuffer ? &blockMesh : nullptr;
    blockMeshBuilt = true;

    struct Face { glm::vec3 normal, right, up; };
    constexpr std::array<Face, 6> faces = {{
        { glm::vec3( 0, 1, 0), glm::vec3( 1, 0, 0), glm::vec3( 0, 0,-1) }, // Up
        { glm::vec3( 0,-1, 0), glm::vec3( 1, 0, 0), glm::vec3( 0, 0, 1) }, // Down
        { glm::vec3( 0, 0,-1), glm::vec3(-1, 0, 0), glm::vec3( 0, 1, 0) }, // North
        { glm::vec3( 0, 0, 1), glm::vec3( 1, 0, 0), glm::vec3( 0, 1, 0) }, // South
        { glm::vec3( 1, 0, 0), glm::vec3( 0, 0,-1), glm::vec3( 0, 1, 0) }, // East
        { glm::vec3(-1, 0, 0), glm::vec3( 0, 0, 1), glm::vec3( 0, 1, 0) }, // West
    }};

    std::vector<EntityJsonModel::Vertex> vertices;
    std::vector<uint32_t> indices;
    const glm::vec3 center(0.0f, 0.5f, 0.0f);
    for (size_t f = 0; f < faces.size(); f++)
    {
        const Face& face = faces[f];
        float u0 = static_cast<float>(f) / 6.0f;
        float u1 = static_cast<float>(f + 1) / 6.0f;
        glm::vec3 faceCenter = center + face.normal * 0.5f;

        uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.push_back({ faceCenter - 0.5f * face.right - 0.5f * face.up, face.normal, { u0, 1.0f } });
        vertices.push_back({ faceCenter + 0.5f * face.right - 0.5f * face.up, face.normal, { u1, 1.0f } });
        vertices.push_back({ faceCenter + 0.5f * face.right + 0.5f * face.up, face.normal, { u1, 0.0f } });
        vertices.push_back({ faceCenter - 0.5f * face.right + 0.5f * face.up, face.normal, { u0, 0.0f } });
        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }

    D3D11_BUFFER_DESC desc{};
    D3D11_SUBRESOURCE_DATA data{};
    desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(EntityJsonModel::Vertex));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    data.pSysMem = vertices.data();
    if (FAILED(device->CreateBuffer(&desc, &data, &blockMesh.vertexBuffer))) return nullptr;
    desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = indices.data();
    if (FAILED(device->CreateBuffer(&desc, &data, &blockMesh.indexBuffer)))
    {
        blockMesh.vertexBuffer.Reset();
        return nullptr;
    }
    blockMesh.indexCount = static_cast<uint32_t>(indices.size());
    return &blockMesh;
}

const D3D11EntityAssets::Skin* D3D11EntityAssets::BlockTexture(int32_t stateId)
{
    if (auto it = blockTextures.find(stateId); it != blockTextures.end())
    {
        return it->second.view ? &it->second : nullptr;
    }
    if (blockTextures.size() >= MAX_BLOCK_TEXTURES) return nullptr;

    Block block = BlockRegistry::ResolveStateId(stateId);
    constexpr int stripWidth = BLOCK_TILE_SIZE * 6;
    std::vector<uint8_t> strip(static_cast<size_t>(stripWidth) * BLOCK_TILE_SIZE * 4, 0);

    bool anyFace = false;
    for (int face = 0; face < 6; face++)
    {
        std::string name = BlockFaceTextureName(block, face);
        if (name.empty()) continue;

        int width = 0, height = 0;
        uint8_t* pixels = LoadImageRGBA("assets/minecraft/textures/block/" + name + ".png", width, height);
        if (!pixels) continue;

        // Animated textures are tall strips; the first square frame is enough.
        for (int y = 0; y < std::min(height, BLOCK_TILE_SIZE); y++)
        {
            for (int x = 0; x < std::min(width, BLOCK_TILE_SIZE); x++)
            {
                memcpy(&strip[(static_cast<size_t>(y) * stripWidth + face * BLOCK_TILE_SIZE + x) * 4],
                    &pixels[(static_cast<size_t>(y) * width + x) * 4], 4);
            }
        }
        stbi_image_free(pixels);
        anyFace = true;
    }

    Skin texture;
    if (anyFace) CreateSkin(strip.data(), stripWidth, BLOCK_TILE_SIZE, texture);
    auto inserted = blockTextures.emplace(stateId, std::move(texture)).first;
    return inserted->second.view ? &inserted->second : nullptr;
}

const EntityScene::MobModel* D3D11EntityAssets::FindMob(const std::string& entityName) const
{
    auto found = mobs.find(entityName);
    return found != mobs.end() ? &found->second : nullptr;
}

void D3D11EntityAssets::ResolvePlayerLook(Application* state, const std::string& key,
    const EntityScene::MobModel*& model, const EntityScene::SkinBase*& skin)
{
    Application::SkinSource source;
    {
        std::lock_guard<std::mutex> lock(state->skinSourcesMutex);
        auto it = state->skinSources.find(key);
        if (it == state->skinSources.end()) return;
        source = it->second;
    }

    auto loaded = playerSkins.find(key);
    if (loaded == playerSkins.end())
    {
        if (state->skinFetcher != nullptr) state->skinFetcher->Request(key, source.url, source.slim);
        return;
    }

    skin = &loaded->second.skin;
    if (loaded->second.slim)
    {
        auto slim = mobs.find("player_slim");
        if (slim != mobs.end()) model = &slim->second;
    }
}

const D3D11EntityAssets::Skin* D3D11EntityAssets::DefaultSkin()
{
    try
    {
        return CachedSkin("player/wide/steve.png");
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

const EntityScene::SkinBase* D3D11EntityAssets::ArmorSkin(const std::string& path)
{
    auto found = armorSkins.find(path);
    if (found != armorSkins.end()) return found->second;
    const Skin* skin = nullptr;
    try
    {
        skin = CachedSkin(path);
    }
    catch (const std::exception&)
    {
    }
    armorSkins[path] = skin;
    return skin;
}

} // namespace Volcano::Engine::D3D11
