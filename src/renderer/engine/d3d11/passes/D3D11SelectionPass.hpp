#pragma once
#ifndef VOLCANO_ENGINE_D3D11_SELECTION_PASS_H
#define VOLCANO_ENGINE_D3D11_SELECTION_PASS_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>
#include "../../RenderPass.hpp"
#include "../../../misc/SelectionScene.hpp"

namespace Volcano::Engine::D3D11 {

// Direct3D 11 twin of the Vulkan SelectionPass (SelectionRenderer): the
// translucent fill and wireframe outline around the targeted block's shape, and
// the break-crack overlay (destroy_stage_0..9, multiplied over the block). What
// to draw comes from SelectionScene, shared with Vulkan. Records after the
// opaque, entity and non-cubic passes so it blends against what is there, and is
// depth-tested without writing depth. Shaders: selection.* and crack.*.
class D3D11SelectionPass : public RenderPass {
public:
    static constexpr const char* NAME = "Selection";
    static constexpr int CRACK_STAGES = 10;

    D3D11SelectionPass() = delete;
    D3D11SelectionPass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
        : RenderPass(NAME), device(d3dDevice), context(d3dContext) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;
    // The crack textures come from the resource pack.
    void ReloadTextures(const ShaderLibrary& shaders) override;

private:
    // Matches the push-constant blocks of selection.vert (cbuffer b1) and crack.vert.
    struct BoxConstants {
        float worldMin[4];
        float worldMax[4];
        float color[4];
    };

    bool BuildBuffers();
    ID3D11ShaderResourceView* CrackView(int stage);
    void ClearCrackTextures();
    void SetBox(ID3D11Buffer* buffer, const SelectionScene::Box& box, const float* color);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    bool ready = false;
    bool crackReady = false;

    SelectionScene::Frame scene; // Reused so its lists keep their capacity.

    Microsoft::WRL::ComPtr<ID3D11VertexShader> selectionVertex;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> selectionPixel;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> selectionLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cubeVertices;
    Microsoft::WRL::ComPtr<ID3D11Buffer> fillIndices;
    Microsoft::WRL::ComPtr<ID3D11Buffer> lineIndices;
    Microsoft::WRL::ComPtr<ID3D11Buffer> selectionConstants;
    Microsoft::WRL::ComPtr<ID3D11BlendState> alphaBlend;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> crackVertex;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> crackPixel;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> crackLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> crackVertices;
    Microsoft::WRL::ComPtr<ID3D11Buffer> crackIndices;
    Microsoft::WRL::ComPtr<ID3D11Buffer> crackConstants;
    Microsoft::WRL::ComPtr<ID3D11BlendState> multiplyBlend;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> crackSampler;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, CRACK_STAGES> crackViews;
    std::array<bool, CRACK_STAGES> crackTried{};

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState;
};

} // namespace Volcano::Engine::D3D11

#endif
