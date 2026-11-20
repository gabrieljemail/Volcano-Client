#pragma once
#ifndef VOLCANO_ENGINE_D3D11_NON_CUBIC_PASS_H
#define VOLCANO_ENGINE_D3D11_NON_CUBIC_PASS_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include "../../RenderPass.hpp"
#include "../../shared/ChunkCull.hpp"

namespace Volcano::Engine::D3D11 {

// Direct3D 11 twin of the Vulkan NonCubicPass: stairs, slabs, plants,
// fences, ... (Application::nonCubicRenderList, "cutout") followed by water
// (Application::translucentRenderList, depth write off so surfaces blend).
// Both lists are drawn back-to-front per chunk via ChunkCull's sort. No
// indirect-draw or shadow-receiving variants yet. See
// vulkan/passes/NonCubicPass for the reference behavior.
class D3D11NonCubicPass : public RenderPass, public Passes::ChunkCull {
public:
    static constexpr const char* NAME = "NonCubic";
    static constexpr const char* VERTEX_SHADER = "misc.vert";
    static constexpr const char* FRAGMENT_SHADER = "misc.frag";

    D3D11NonCubicPass() = delete;
    D3D11NonCubicPass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext);

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;

private:
    void DrawList(const std::vector<const Mesh*>& meshes, Application* state,
        uint32_t triangleCategory);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> pushBuffer;           // b1, dynamic
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthWrite;   // cutout
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthNoWrite; // translucent
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend;

    Passes::ChunkCull::VisibleCache cutoutCache;
    Passes::ChunkCull::VisibleCache translucentCache;
};

} // namespace Volcano::Engine::D3D11

#endif
