#pragma once
#ifndef VOLCANO_ENGINE_D3D11_ENTITY_ASSETS_H
#define VOLCANO_ENGINE_D3D11_ENTITY_ASSETS_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <string>
#include <unordered_map>
#include <wrl/client.h>
#include "../../../entity/EntityScene.hpp"

namespace Volcano::Engine::D3D11 {

// Direct3D 11 side of the entity assets: textured mob models from mobs.json,
// their skins, downloaded player skins and armor sheets. The Vulkan twin is
// the anonymous-namespace code in entity/EntityRenderer.cpp; the CPU side
// (poses, parts, armor layout) is shared through EntityScene.
class D3D11EntityAssets final : public EntityScene::Assets {
public:
    struct Skin : EntityScene::SkinBase {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    };
    struct Mesh : EntityScene::MeshBase {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
    };

    static const Skin& AsD3D(const EntityScene::SkinBase& skin) { return static_cast<const Skin&>(skin); }
    static const Mesh& AsD3D(const EntityScene::MeshBase& mesh) { return static_cast<const Mesh&>(mesh); }

    explicit D3D11EntityAssets(ID3D11Device* d3dDevice) : device(d3dDevice) {}

    // Reads mobs.json and loads every model and skin it lists. A bad entry is
    // logged and skipped; throws only if nothing at all could be read.
    void Load();
    void Release();

    // Uploads the player skins the fetcher finished (render thread).
    void UploadFinishedSkins(Application* state);

    // Block-shaped entities (falling blocks, TNT): one unit cube whose six
    // faces sample a per-block-state strip texture (Up, Down, North, South,
    // East, West side by side). BlockTexture is null when the state has
    // nothing drawable (or the small cache is full).
    const Mesh* BlockMesh();
    const Skin* BlockTexture(int32_t stateId);

    // The default (Steve) skin, for the first-person arm until the player's own
    // arrives. Null if the pack has none.
    const Skin* DefaultSkin();

    // The sampler every skin uses (point filtered, clamped).
    ID3D11SamplerState* GetSampler() const { return sampler.Get(); }
    void CreateSampler();

    // EntityScene::Assets
    const EntityScene::MobModel* FindMob(const std::string& entityName) const override;
    void ResolvePlayerLook(Application* state, const std::string& key, const EntityScene::MobModel*& model,
        const EntityScene::SkinBase*& skin) override;
    const EntityScene::SkinBase* ArmorSkin(const std::string& path) override;
    const EntityScene::SkinBase* ArmorGlintSkin() override;

    // The glint sheet's own sampler (linear, wrapping: the shader scrolls it).
    ID3D11SamplerState* GetGlintSampler() const { return glintSampler.Get(); }

private:
    const Skin* CachedSkin(const std::string& skinPath);
    const Mesh* CachedMesh(const std::string& modelFile, float scale);
    bool CreateSkin(const uint8_t* rgba, int width, int height, Skin& out);

    ID3D11Device* device = nullptr;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;

    // Node-based maps: the pointers MobModel holds stay valid as they grow.
    std::unordered_map<std::string, Skin> skinCache;
    std::unordered_map<std::string, Mesh> meshCache;
    std::unordered_map<std::string, EntityScene::MobModel> mobs;

    struct PlayerSkin {
        Skin skin;
        bool slim = false;
    };
    std::unordered_map<std::string, PlayerSkin> playerSkins;
    std::unordered_map<std::string, const Skin*> armorSkins; // Null where the packs have none.

    Skin glintSkin;
    bool glintTried = false;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> glintSampler;

    Mesh blockMesh;
    bool blockMeshBuilt = false;
    std::unordered_map<int32_t, Skin> blockTextures; // An entry with a null view means "no usable texture".
};

} // namespace Volcano::Engine::D3D11

#endif
