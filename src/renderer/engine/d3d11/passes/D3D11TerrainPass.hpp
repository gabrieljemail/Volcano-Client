#pragma once
#ifndef VOLCANO_ENGINE_D3D11_TERRAIN_PASS_H
#define VOLCANO_ENGINE_D3D11_TERRAIN_PASS_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include "../../RenderPass.hpp"
#include "../../shared/ChunkCull.hpp"

namespace Volcano::Engine::D3D11 {

// Direct3D 11 twin of the Vulkan TerrainPass: opaque full-cube chunk
// geometry (Application::renderList), frustum-culled per chunk via the shared
// ChunkCull. No indirect-draw path yet - Graphics.IndirectDraw is ignored
// here, same as a Vulkan device without the feature. See
// vulkan/passes/TerrainPass for the reference behavior.
class D3D11TerrainPass : public RenderPass, public Passes::ChunkCull {
public:
    static constexpr const char* NAME = "Terrain";
    static constexpr const char* VERTEX_SHADER = "terrain.vert";
    static constexpr const char* FRAGMENT_SHADER = "terrain.frag";

    D3D11TerrainPass() = delete;
    D3D11TerrainPass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext);

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;

private:
    // Borrowed from the engine; passes are destroyed before the device.
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> pushBuffer;          // b1, dynamic
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerSolid;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerWireframe;

    Passes::ChunkCull::VisibleCache cache;
};

} // namespace Volcano::Engine::D3D11

#endif
