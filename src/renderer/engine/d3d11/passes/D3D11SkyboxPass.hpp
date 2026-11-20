#pragma once
#ifndef VOLCANO_ENGINE_D3D11_SKYBOX_PASS_H
#define VOLCANO_ENGINE_D3D11_SKYBOX_PASS_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include "../../RenderPass.hpp"

namespace Volcano::Engine::D3D11 {

// Direct3D 11 twin of the Vulkan SkyboxPass: the background cube, drawn
// first so everything else draws over it. In menus (SceneView::menuBackground)
// it's the slowly rotating title panorama; in game it's the sky gradient with
// sun and moon billboards. See vulkan/passes/SkyboxPass for the shared logic.
class D3D11SkyboxPass : public RenderPass {
public:
    static constexpr const char* NAME = "Skybox";
    static constexpr const char* VERTEX_SHADER = "skybox.vert";
    static constexpr const char* FRAGMENT_SHADER = "skybox.frag";

    D3D11SkyboxPass() = delete;
    D3D11SkyboxPass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext);

    void Create(const ShaderLibrary& shaders) override;
    void Record(FrameContext& frame) override;
    void Destroy() override;

    // Only the pipeline state depends on shaders; the textures survive.
    void ReloadShaders(const ShaderLibrary& shaders) override;

    // Reloads the panorama and sun/moon art (full rebuild; GPU is idle).
    void ReloadTextures(const ShaderLibrary& shaders) override;

private:
    // Borrowed from the engine; passes are destroyed before the device.
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    bool LoadCubemap();
    bool LoadCelestial();
    // Uploads `layers` RGBA8 images of size x size into a new texture + SRV
    // (a cubemap when cube, a 2D array otherwise).
    bool UploadLayers(const std::vector<const uint8_t*>& layers, uint32_t size, bool cube,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv);
    void CreatePipeline();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> cubeTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cubeSRV;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> cubeSampler;

    // Sun (layer 0) and the 8 moon phases (layers 1-8), all one size.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> celestialTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> celestialSRV;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> celestialSampler;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> pushBuffer;      // b0, dynamic
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState; // depth off
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
};

} // namespace Volcano::Engine::D3D11

#endif
