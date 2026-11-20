#pragma once
#ifndef VOLCANO_ENGINE_D3D11_PARTICLE_PASS_H
#define VOLCANO_ENGINE_D3D11_PARTICLE_PASS_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <d3d11.h>
#include <vector>
#include <wrl/client.h>
#include "../../RenderPass.hpp"
#include "../../../particle/ParticleSystem.hpp"

namespace Volcano::Engine::D3D11 {

// Direct3D 11 twin of the Vulkan ParticlePass: Application::particles, one
// instance of a camera-facing quad per particle, sampled from the particle
// atlas (ParticleSprites) as a 2D array. ParticleSystem already does the
// ticking and builds the instances for either backend; this uploads them and
// draws, blended, depth-tested and not depth-writing. Runs the particles' ticks
// too, since it is where the frame's time passes. Setting: Graphics.Particles.
class D3D11ParticlePass : public RenderPass {
public:
    static constexpr const char* NAME = "Particles";
    static constexpr const char* VERTEX_SHADER = "particle.vert";
    static constexpr const char* FRAGMENT_SHADER = "particle.frag";

    D3D11ParticlePass() = delete;
    D3D11ParticlePass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
        : RenderPass(NAME), device(d3dDevice), context(d3dContext) {}

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;
    void ReloadTextures(const ShaderLibrary& shaders) override;

private:
    // Matches particle.vert's Push block (cbuffer b0).
    struct Push {
        float viewProj[16];
        float right[4];
        float up[4];
    };

    bool UploadAtlas(const ParticleSprites& sprites);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    bool atlasFailed = false;
    std::vector<ParticleInstance> scratch;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> instanceBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> atlasView;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
};

} // namespace Volcano::Engine::D3D11

#endif
