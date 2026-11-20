#pragma once
#ifndef VOLCANO_ENGINE_D3D11_ENTITY_PASS_H
#define VOLCANO_ENGINE_D3D11_ENTITY_PASS_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <vector>
#include <wrl/client.h>
#include "../../RenderPass.hpp"
#include "../../../entity/EntityScene.hpp"
#include "D3D11EntityAssets.hpp"

namespace Volcano::Engine::D3D11 {

class D3D11RenderEngine;
class D3D11ItemRenderer;

// Direct3D 11 twin of the Vulkan EntityPass. What gets drawn, and how it is
// posed, comes from EntityScene (shared with Vulkan). Two paths:
//   * flat-tinted shapes (entity.vert/frag): the placeholder box, quadruped,
//     golem, pig and cow meshes, one instanced draw per shape;
//   * textured models (entity_textured.vert/frag): mobs.json models with their
//     skins - players (with their downloaded skins), the local player's own
//     body, mobs and worn armor - one instanced draw per (mesh, skin, part).
//
// Held items on other players go through D3D11ItemRenderer; the first-person hand
// and dropped items are their own passes. Not ported yet: chests and other modeled
// block entities, and entity shadows. Armor, its enchantment glint and falling-block
// textures are drawn. Without mobs.json models (e.g. missing assets) every entity falls
// back to its flat shape.
class D3D11EntityPass : public RenderPass {
public:
    static constexpr const char* NAME = "Entities";
    static constexpr const char* VERTEX_SHADER = "entity.vert";
    static constexpr const char* FRAGMENT_SHADER = "entity.frag";

    // Matches EntityRenderer::MAX_VISIBLE_ENTITIES.
    static constexpr uint32_t MAX_VISIBLE_ENTITIES = 256;

    static constexpr const char* TEXTURED_VERTEX_SHADER = "entity_textured.vert";
    static constexpr const char* TEXTURED_FRAGMENT_SHADER = "entity_textured.frag";
    static constexpr const char* GLINT_FRAGMENT_SHADER = "entity_glint.frag";

    D3D11EntityPass() = delete;
    D3D11EntityPass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext);

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;

private:
    // Order matches the Vulkan EntityShapeSlot enum.
    enum Shape : uint32_t { SHAPE_CUBE, SHAPE_QUADRUPED, SHAPE_GOLEM, SHAPE_PIG, SHAPE_COW, SHAPE_COUNT };

    // One mesh plus its own instance buffer. D3D11's SV_InstanceID doesn't
    // include StartInstanceLocation the way Vulkan's gl_InstanceIndex includes
    // firstInstance, so each shape reads its instances from a separate buffer
    // instead of a slice of a shared one.
    struct ShapeResources {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        uint32_t indexCount = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> instanceBuffer;             // raw, t0 in the vertex shader
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> instanceView;
    };

    void CreateShape(Shape shape, const void* vertices, size_t vertexBytes,
        const std::vector<uint32_t>& indices);

    void CreateTexturedPath();
    void DrawTextured(D3D11RenderEngine* d3d, Application* state, const EntityScene::Frame& scene);

    // One textured part waiting to be batched.
    struct TexturedItem {
        const D3D11EntityAssets::Mesh* mesh;
        const D3D11EntityAssets::Skin* skin;
        uint32_t indexCount;
        uint32_t firstIndex;
        EntityScene::TexturedInstance instance;
    };
    static constexpr uint32_t MAX_TEXTURED_INSTANCES = 8192;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    // Textured path; null `assets` means it is unavailable and everything draws flat.
    std::unique_ptr<D3D11EntityAssets> assets;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> texturedVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> texturedPixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> texturedLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> texturedInstanceBuffer; // Slot 1, dynamic.
    std::vector<TexturedItem> texturedItems;
    std::vector<TexturedItem> glintItems;
    // Armor enchantment glint: the armor again, additively, over its own depth.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> glintPixelShader;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> glintDepthState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> glintBlendState;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;

    std::array<ShapeResources, SHAPE_COUNT> shapes;

    // Held items on other players ride on the posed arms (EntityScene::CollectHeldItems).
    std::vector<EntityScene::HeldItemDraw> heldItems;
    D3D11ItemRenderer* registeredItems = nullptr; // Has our assets for the first-person hand.
};

} // namespace Volcano::Engine::D3D11

#endif
