#pragma once
#ifndef VOLCANO_ENGINE_D3D11_RENDER_ENGINE_H
#define VOLCANO_ENGINE_D3D11_RENDER_ENGINE_H

#include <atomic>
#include <cstdint>
#include "../RenderEngine.hpp"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;
struct ID3D11DepthStencilView;
struct ID3D11ShaderResourceView;
struct ID3D11SamplerState;
struct ID3D11Buffer;
struct HWND__;

namespace Volcano::Engine::D3D11 {

class D3D11RenderEngine : public RenderEngine {
public:
    explicit D3D11RenderEngine(Application* application) : RenderEngine(application) {}

    void Init() override;
    void AttachTextures(TextureManager& textures) override;
    void AttachGUI(const TextureManager* textures) override;
    void StopRendering() override;
    void Shutdown() override;
    const char* GetBackendName() const override { return "Direct3D 11"; }
    D3D11RenderEngine* GetD3D11() override { return this; }

    ID3D11Device* GetD3D11Device() const { return device; }
    ID3D11DeviceContext* GetImmediateContext() const { return context; }
    IDXGISwapChain* GetSwapChain() const { return swapChain; }
    ID3D11ShaderResourceView* GetBlockTextureArray() const { return blockArraySRV; }
    ID3D11SamplerState* GetBlockTextureSampler() const { return blockArraySampler; }

    // The per-frame camera constants (CameraUBO), already uploaded for this
    // frame when Record() runs. Bind at slot b0 - the HLSL twins declare it
    // as `cbuffer CameraUBO : register(b0)`.
    ID3D11Buffer* GetCameraConstantBuffer() const { return cameraBuffer; }

    bool BeginFrame() override;
    void SubmitFrame(const SceneView& scene) override;
    RenderExtent GetExtent() const override { return extent; }
    uint32_t GetFramesInFlight() const override { return 1; }
    void WaitIdle() override;

    void ToggleWindowMode() override;
    void ApplyWindowSettings() override;
    void RequestScreenshot() override { screenshotRequested.store(true); }

private:
    void CreateBackBufferResources();
    void ReleaseBackBufferResources();
    void Resize(uint32_t width, uint32_t height);
    void CaptureScreenshot();
    void UploadCamera(const SceneView& scene);
    void ApplyWindowMode(uint32_t mode, uint32_t fullscreenWidth, uint32_t fullscreenHeight,
        uint32_t refreshRate);

    // DXGI_SWAP_CHAIN_FLAGs shared by creation and ResizeBuffers (they must match).
    uint32_t SwapChainFlags() const;
    // Caps how many frames the CPU may run ahead of presentation to
    // Graphics.BufferCount (DXGI's default of 3 lets a cheap frame, like a
    // menu, queue ~3 frames of input lag), and refreshes the waitable handle.
    void ConfigureFrameLatency();

    HWND__* hwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* backBufferRTV = nullptr;
    ID3D11Texture2D* depthTexture = nullptr;
    ID3D11DepthStencilView* depthView = nullptr;
    ID3D11ShaderResourceView* blockArraySRV = nullptr;
    ID3D11SamplerState* blockArraySampler = nullptr;
    ID3D11Buffer* cameraBuffer = nullptr;
    void* frameLatencyWaitable = nullptr; // HANDLE; signaled when a new frame may be started
    bool firstFramePresented = false;

    RenderExtent extent{};
    std::atomic<bool> screenshotRequested{false};
    uint32_t bufferCount = 2;
    int windowedX = 0;
    int windowedY = 0;
    uint32_t windowedWidth = 854;
    uint32_t windowedHeight = 480;
    uint32_t windowMode = 0;
    uint32_t fullscreenWidth = 0;
    uint32_t fullscreenHeight = 0;
    uint32_t refreshRate = 0;
    uint64_t appliedConfigVersion = UINT64_MAX;
    bool vsync = true;
    bool tearingSupported = false;
    bool flipModel = false; // False when the legacy blit swap effect had to be used.
    bool initialized = false;
    bool guiAttached = false;
};

} // namespace Volcano::Engine::D3D11

#endif
