#pragma once
#ifndef VOLCANO_ENGINE_D3D11_HAND_PASS_H
#define VOLCANO_ENGINE_D3D11_HAND_PASS_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include "../../RenderPass.hpp"
#include "../../../misc/HandScene.hpp"

namespace Volcano::Engine::D3D11 {

// Direct3D 11 twin of the Vulkan HandPass: the first-person arm and held items,
// drawn over everything else in the world. Placement and animation come from
// HandScene (shared with Vulkan); the arm boxes use the local player's skin from
// the entity pass's assets (via D3D11ItemRenderer), and held items go through
// D3D11ItemRenderer. Records after the entity pass.
class D3D11HandPass : public RenderPass {
public:
    static constexpr const char* NAME = "Hand";
    static constexpr const char* VERTEX_SHADER = "hand.vert";
    static constexpr const char* FRAGMENT_SHADER = "hand.frag";

    D3D11HandPass() = delete;
    D3D11HandPass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
        : RenderPass(NAME), device(d3dDevice), context(d3dContext) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;

private:
    struct ArmMesh {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertices;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indices;
        uint32_t indexCount = 0;
    };
    // Matches hand.vert's PushConstants block (cbuffer b0).
    struct ArmConstants {
        float mvp[16];
        float shade[4];
    };

    bool BuildArm(ArmMesh& mesh, const glm::vec3& boxMin, const glm::vec2& uv);
    void DrawArm(const ArmMesh& mesh, const glm::mat4& mvp, float brightness, ID3D11ShaderResourceView* skin,
        ID3D11SamplerState* sampler);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    bool ready = false;

    HandScene scene; // Animation state, shared with the Vulkan hand.
    HandScene::Frame hands;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
    ArmMesh rightArm;
    ArmMesh leftArm;
};

} // namespace Volcano::Engine::D3D11

#endif
