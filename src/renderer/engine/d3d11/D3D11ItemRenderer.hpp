#pragma once
#ifndef VOLCANO_ENGINE_D3D11_ITEM_RENDERER_H
#define VOLCANO_ENGINE_D3D11_ITEM_RENDERER_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <d3d11.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#include <glm/glm.hpp>

namespace Volcano {
class TextureManager;
class ItemModel;
struct ItemDrawParams;
} // namespace Volcano

namespace Volcano::Engine::D3D11 {

class D3D11RenderEngine;
class D3D11EntityAssets;

// Direct3D 11 twin of ItemRenderer: draws ItemModels with the block/item
// texture array. What to draw and where (display transforms, constants) is
// ItemDraw.hpp, shared with Vulkan. It serves three users:
//   * the world: dropped and thrown items, held items on other players, and
//     the first-person hand's item (DrawItem, inside the main pass);
//   * the inventory: model icons for blocks and custom models, rendered once
//     into a small texture (GuiIconView for ImGui, GuiIconPixels for Pincel,
//     which wants CPU pixels), and flat item sprites (FlatIconView);
//   * the hand pass, which borrows the entity pass's assets for the skin.
//
// Render thread only (it uses the immediate context).
class D3D11ItemRenderer {
public:
    static constexpr uint32_t GUI_ICON_SIZE = 64;

    D3D11ItemRenderer(D3D11RenderEngine* renderEngine, const TextureManager* textureManager)
        : engine(renderEngine), textures(textureManager) {}

    // Needs the texture array attached (RenderEngine::AttachTextures). Throws
    // std::runtime_error if the shaders or states can't be created.
    void Init();
    void Shutdown();
    // Frees the cached meshes and icons (they bake texture layer indices)
    // after a resource reload; they are rebuilt on demand.
    void ReloadTextures();

    bool HasModel(const std::string& itemName) const;

    // Draws an item inside the main pass: `placement` * the model's display
    // transform for `context` ("ground", "firstperson_righthand", ...), then
    // `viewProj`. `pivotRotation` turns it in place about its own center,
    // `darkness` takes light away (0 to 1), `glint` adds the enchantment sheet.
    void DrawItem(const std::string& itemName, const std::string& context, const glm::mat4& placement,
        const glm::mat4& viewProj, const glm::mat4& pivotRotation = glm::mat4(1.0f), float darkness = 0.0f,
        bool glint = false);

    // The model icon as a GUI_ICON_SIZE texture (owned here, valid until
    // ReloadTextures/Shutdown), or null if the item has no 3D model.
    ID3D11ShaderResourceView* GuiIconView(const std::string& itemName, bool glint);
    // The same icon as RGBA8 pixels, straight alpha.
    bool GuiIconPixels(const std::string& itemName, bool glint, std::vector<uint8_t>& rgba);

    // One layer of the block/item texture array as a plain 2D texture (owned
    // here), for a flat item's icon. Null for an unknown layer.
    ID3D11ShaderResourceView* FlatIconView(uint32_t layer);

    // The entity pass registers its assets so the hand can borrow the local
    // player's skin; null again when that pass is destroyed.
    void SetEntityAssets(D3D11EntityAssets* assets) { entityAssets = assets; }
    D3D11EntityAssets* GetEntityAssets() const { return entityAssets; }

    ID3D11Device* GetDevice() const { return device.Get(); }
    ID3D11DeviceContext* GetContext() const { return context.Get(); }

private:
    struct Mesh {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertices;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indices;
        uint32_t indexCount = 0;
    };
    struct Icon {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    };

    const Mesh* MeshFor(const std::string& itemName, const ItemModel& model);
    void Upload(const ItemDrawParams& draw);
    // Binds shaders and states shared by every item draw (opaque, then glint).
    void BindOpaque();
    void BindGlint();
    void DrawMesh(const Mesh& mesh);
    bool LoadGlintSheet();
    Icon* RenderIcon(const std::string& itemName, bool glint);

    D3D11RenderEngine* engine = nullptr;
    const TextureManager* textures = nullptr;
    D3D11EntityAssets* entityAssets = nullptr;
    bool initialized = false;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> glintPixelShader; // Null if item_glint.frag failed to build.
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> glintDepthState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> glintBlendState;

    // The enchantment sheet and its (linear, wrapping) sampler; tried once.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> glintSheet;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> glintSampler;
    bool glintSheetTried = false;

    // GUI icon target depth, shared (each render clears it).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> iconDepth;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> iconDepthView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> iconStaging;

    std::unordered_map<std::string, Mesh> meshes;
    std::unordered_map<std::string, Icon> icons; // Keyed by name + glint flag.
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> flatIcons;
};

} // namespace Volcano::Engine::D3D11

#endif
