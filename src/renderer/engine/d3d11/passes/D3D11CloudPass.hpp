#pragma once
#ifndef VOLCANO_ENGINE_D3D11_CLOUD_PASS_H
#define VOLCANO_ENGINE_D3D11_CLOUD_PASS_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <d3d11.h>
#include <vector>
#include <wrl/client.h>
#include "../../RenderPass.hpp"
#include "../../../misc/CloudScene.hpp"

namespace Volcano::Engine::D3D11 {

// Direct3D 11 twin of the Vulkan CloudPass: vanilla's drifting cloud grid, built
// camera-relative on the CPU whenever the camera crosses a cell. The cells, the
// mesh and the per-frame placement and color are CloudScene (shared with
// Vulkan); this owns the vertex buffer and draws it, blended, depth-tested but
// not depth-writing. Settings: Graphics.Clouds, Graphics.CloudHeight.
class D3D11CloudPass : public RenderPass {
public:
    static constexpr const char* NAME = "Clouds";
    static constexpr const char* VERTEX_SHADER = "clouds.vert";
    static constexpr const char* FRAGMENT_SHADER = "clouds.frag";

    D3D11CloudPass() = delete;
    D3D11CloudPass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
        : RenderPass(NAME), device(d3dDevice), context(d3dContext) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;
    void ReloadTextures(const ShaderLibrary& shaders) override;

private:
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    CloudScene scene;
    std::vector<CloudScene::Vertex> scratch; // The mesh as built, before it is copied into the GPU buffer.
    uint32_t vertexCount = 0;
    int centerX = 0, centerZ = 0;
    int mode = -1; // 1 fast, 2 fancy; -1 = the buffer holds nothing current

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
};

} // namespace Volcano::Engine::D3D11

#endif
