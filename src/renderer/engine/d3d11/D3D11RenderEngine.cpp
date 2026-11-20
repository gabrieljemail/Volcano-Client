#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include "D3D11RenderEngine.hpp"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <stb_image.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../../../ApplicationDeps.hpp"
import Volcano;
#include "../../../Logger.hpp"
#include "../../../platform/OS.hpp"
#include "../../Screenshot.hpp"
#include "../../models/CameraUBO.hpp"
#include "../../models/SlabBuffer.hpp"
#include "../../models/SlabBufferD3D11.hpp"
#include "../../terrain/ChunkMesher.hpp"
#include "../../misc/NonCubicMesher.hpp"
#undef CreateWindow
#include "../../gui/GUIController.hpp"
#include "passes/D3D11SkyboxPass.hpp"
#include "passes/D3D11TerrainPass.hpp"
#include "passes/D3D11NonCubicPass.hpp"
#include "passes/D3D11EntityPass.hpp"
#include "passes/D3D11ItemPass.hpp"
#include "passes/D3D11HandPass.hpp"
#include "passes/D3D11SelectionPass.hpp"
#include "passes/D3D11CloudPass.hpp"
#include "passes/D3D11ParticlePass.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace Volcano::Engine::D3D11 {

namespace {

Application* g_windowState = nullptr;

std::string HResultMessage(const char* operation, HRESULT result)
{
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
    return std::string("[ERROR] ") + operation + " failed (" + code + ").";
}

void ThrowIfFailed(HRESULT result, const char* operation)
{
    if (FAILED(result)) throw std::runtime_error(HResultMessage(operation, result));
}

std::string ToUtf8(const wchar_t* wide)
{
    if (wide == nullptr || *wide == L'\0') return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

void FramebufferSizeCallback(GLFWwindow*, int width, int height)
{
    if (g_windowState == nullptr) return;
    g_windowState->pendingFramebufferWidth = width;
    g_windowState->pendingFramebufferHeight = height;
    g_windowState->framebufferResized = true;
}

void WindowFocusCallback(GLFWwindow* window, int focused)
{
    if (g_windowState == nullptr) return;
    g_windowState->windowFocused.store(focused != 0);
    if (!focused) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

HRESULT CreateDeviceForAdapter(IDXGIAdapter1* adapter, ID3D11Device** device,
    D3D_FEATURE_LEVEL* selectedLevel, ID3D11DeviceContext** context)
{
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    HRESULT result = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
        nullptr, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device, selectedLevel, context);
    if (result == E_INVALIDARG)
    {
        const D3D_FEATURE_LEVEL fallbackLevels[] = { D3D_FEATURE_LEVEL_11_0 };
        result = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
            nullptr, 0, fallbackLevels, ARRAYSIZE(fallbackLevels), D3D11_SDK_VERSION, device, selectedLevel, context);
    }
    return result;
}

const char* FeatureLevelName(D3D_FEATURE_LEVEL level)
{
    switch (level)
    {
        case D3D_FEATURE_LEVEL_11_1: return "11.1";
        case D3D_FEATURE_LEVEL_11_0: return "11.0";
        default: return "unknown";
    }
}

} // namespace

void D3D11RenderEngine::Init()
{
    if (!glfwInit()) throw std::runtime_error("[ERROR] Failed to initialize GLFW for Direct3D 11.");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);

    g_windowState = state;
    windowedWidth = std::get<uint32_t>(state->config->Get("Window.Width", uint32_t{854}));
    windowedHeight = std::get<uint32_t>(state->config->Get("Window.Height", uint32_t{480}));
    state->window = glfwCreateWindow(static_cast<int>(windowedWidth), static_cast<int>(windowedHeight),
        "Volcano Client", nullptr, nullptr);
    if (state->window == nullptr) throw std::runtime_error("[ERROR] Failed to create the Direct3D 11 window.");

    hwnd = reinterpret_cast<HWND__*>(glfwGetWin32Window(state->window));
    glfwGetWindowPos(state->window, &windowedX, &windowedY);
    glfwSetFramebufferSizeCallback(state->window, FramebufferSizeCallback);
    glfwSetWindowFocusCallback(state->window, WindowFocusCallback);

    int iconWidth = 0;
    int iconHeight = 0;
    int iconChannels = 0;
    unsigned char* iconPixels = stbi_load("resources/logo.png", &iconWidth, &iconHeight, &iconChannels, 4);
    if (iconPixels != nullptr)
    {
        GLFWimage icon{ iconWidth, iconHeight, iconPixels };
        glfwSetWindowIcon(state->window, 1, &icon);
        stbi_image_free(iconPixels);
    }

    ComPtr<IDXGIFactory1> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    ComPtr<IDXGIAdapter1> selectedAdapter;
    SIZE_T selectedMemory = 0;
    Log::Info("DXGI devices present:");
    for (UINT index = 0; ; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        ThrowIfFailed(result, "IDXGIFactory1::EnumAdapters1");

        DXGI_ADAPTER_DESC1 desc{};
        ThrowIfFailed(adapter->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;

        SIZE_T memory = desc.DedicatedVideoMemory + desc.SharedSystemMemory;
        Log::Info("  " + ToUtf8(desc.Description) + " ("
            + std::to_string(static_cast<uint64_t>(memory / (1024 * 1024))) + " MiB reported memory)");
        if (!selectedAdapter || memory > selectedMemory)
        {
            selectedAdapter = adapter;
            selectedMemory = memory;
        }
    }

    D3D_FEATURE_LEVEL selectedLevel{};
    HRESULT deviceResult = selectedAdapter
        ? CreateDeviceForAdapter(selectedAdapter.Get(), &device, &selectedLevel, &context)
        : E_FAIL;
    if (FAILED(deviceResult))
    {
        if (selectedAdapter)
        {
            Log::Info("Hardware D3D11 device creation failed; trying the Windows WARP renderer.");
            selectedAdapter.Reset();
        }
        deviceResult = CreateDeviceForAdapter(nullptr, &device, &selectedLevel, &context);
    }
    ThrowIfFailed(deviceResult, "D3D11CreateDevice");

    // The meshing thread Map()s slab buffers while this thread records draws;
    // multithread protection lets the runtime serialize the immediate context.
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread))))
    {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
    SlabBufferD3D11::SetDeviceContext(device, context);
    SetSlabBufferBackend(SlabBufferBackend::D3D11);

    bufferCount = std::clamp(std::get<uint32_t>(state->config->Get("Graphics.BufferCount", uint32_t{2})), 2u, 3u);
    vsync = std::get<bool>(state->config->Get("Graphics.VSync", true));

    // Tearing (vsync off without waiting for the compositor) is a flip-model
    // feature that needs DXGI 1.5; absent on older Windows 10 builds.
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5)))
    {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allowTearing, sizeof(allowTearing))))
        {
            tearingSupported = allowTearing != FALSE;
        }
    }

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferDesc.Width = windowedWidth;
    swapDesc.BufferDesc.Height = windowedHeight;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = bufferCount;
    swapDesc.OutputWindow = reinterpret_cast<HWND>(hwnd);
    swapDesc.Windowed = TRUE;
    // Flip model lets the compositor scan out our buffers directly (lower
    // latency, less DWM overhead), but FLIP_DISCARD needs Windows 10 and
    // FLIP_SEQUENTIAL Windows 8: older systems and some drivers refuse them, so
    // fall back to the legacy blit model instead of failing to start. Flip
    // models need BufferCount >= 2, which the clamp above guarantees.
    static constexpr DXGI_SWAP_EFFECT EFFECTS[] = {
        DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SWAP_EFFECT_DISCARD };
    const bool tearingAvailable = tearingSupported;
    HRESULT swapResult = E_FAIL;
    for (DXGI_SWAP_EFFECT effect : EFFECTS)
    {
        flipModel = effect != DXGI_SWAP_EFFECT_DISCARD;
        tearingSupported = flipModel && tearingAvailable; // Tearing is a flip-model feature.
        swapDesc.SwapEffect = effect;
        swapDesc.Flags = SwapChainFlags();
        swapResult = factory->CreateSwapChain(device, &swapDesc, &swapChain);
        if (SUCCEEDED(swapResult)) break;
        Log::Info(std::string("Swap effect ") + (effect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "FLIP_DISCARD"
            : effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ? "FLIP_SEQUENTIAL" : "DISCARD")
            + " unavailable; trying the next one.");
        swapChain = nullptr;
    }
    ThrowIfFailed(swapResult, "IDXGIFactory1::CreateSwapChain");
    if (!flipModel) Log::Info("Using the legacy swap model: input latency will be higher than on Windows 10.");
    factory->MakeWindowAssociation(reinterpret_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER);
    ConfigureFrameLatency();

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(state->window, &framebufferWidth, &framebufferHeight);
    if (framebufferWidth <= 0 || framebufferHeight <= 0)
    {
        framebufferWidth = static_cast<int>(windowedWidth);
        framebufferHeight = static_cast<int>(windowedHeight);
    }
    extent = { static_cast<uint32_t>(framebufferWidth), static_cast<uint32_t>(framebufferHeight) };
    CreateBackBufferResources();

    D3D11_BUFFER_DESC cameraDesc{};
    cameraDesc.ByteWidth = (sizeof(CameraUBO) + 15u) & ~15u;
    cameraDesc.Usage = D3D11_USAGE_DYNAMIC;
    cameraDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cameraDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ThrowIfFailed(device->CreateBuffer(&cameraDesc, nullptr, &cameraBuffer),
        "ID3D11Device::CreateBuffer(camera)");

    AddPass(std::make_unique<D3D11SkyboxPass>(device, context));
    AddPass(std::make_unique<D3D11TerrainPass>(device, context));
    AddPass(std::make_unique<D3D11EntityPass>(device, context)); // Vulkan order: terrain, entities, non-cubic
    AddPass(std::make_unique<D3D11NonCubicPass>(device, context));
    AddPass(std::make_unique<D3D11ItemPass>()); // Vulkan order: ... items ... hand (last, over everything)
    AddPass(std::make_unique<D3D11ParticlePass>(device, context)); // Vulkan order: items, non-cubic, particles, clouds, selection, hand
    AddPass(std::make_unique<D3D11CloudPass>(device, context));
    AddPass(std::make_unique<D3D11SelectionPass>(device, context));
    AddPass(std::make_unique<D3D11HandPass>(device, context));
    CreatePasses();
    initialized = true;
    Log::Info(std::string("[INFO] Render engine initialized (") + GetBackendName()
        + ", feature level " + FeatureLevelName(selectedLevel) + ", " + std::to_string(extent.width) + "x"
        + std::to_string(extent.height) + ").");
}

void D3D11RenderEngine::AttachTextures(TextureManager& textures)
{
    if (!device || !context) throw std::runtime_error("[ERROR] Direct3D 11 texture upload requested without a device.");
    if (textures.array.layerCount == 0 || textures.LayerSize() == 0)
    {
        throw std::runtime_error("[ERROR] Cannot upload an empty Direct3D 11 texture array.");
    }

    // D3D11 caps a texture array at 2048 slices (and mip counts must match
    // TextureManager's). Past the cap, keep the first 2048 layers and say so,
    // instead of failing to start: the rest sample as empty.
    uint32_t layerCount = textures.array.layerCount;
    if (layerCount > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
    {
        Log::Error("[WARN] " + std::to_string(layerCount) + " block textures exceed Direct3D 11's "
            + std::to_string(D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
            + "-layer limit; the extra ones will not render. Use fewer or smaller resource packs, or the Vulkan renderer.");
        layerCount = D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = textures.LayerSize();
    textureDesc.Height = textures.LayerSize();
    textureDesc.MipLevels = 0;
    textureDesc.ArraySize = layerCount;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ComPtr<ID3D11Texture2D> texture;
    ThrowIfFailed(device->CreateTexture2D(&textureDesc, nullptr, &texture),
        "ID3D11Device::CreateTexture2D(texture array)");
    const uint32_t mipLevels = textures.MipLevels();
    for (uint32_t layer = 0; layer < layerCount; ++layer)
    {
        const uint8_t* pixels = textures.GetLayerPixels(layer);
        if (pixels == nullptr) throw std::runtime_error("[ERROR] Texture array layer has no CPU pixels.");
        context->UpdateSubresource(texture.Get(), D3D11CalcSubresource(0, layer, mipLevels),
            nullptr, pixels, textures.LayerSize() * 4, textures.LayerSize() * textures.LayerSize() * 4);
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = textureDesc.Format;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    viewDesc.Texture2DArray.MostDetailedMip = 0;
    viewDesc.Texture2DArray.MipLevels = mipLevels;
    viewDesc.Texture2DArray.FirstArraySlice = 0;
    viewDesc.Texture2DArray.ArraySize = layerCount;
    ComPtr<ID3D11ShaderResourceView> newSRV;
    ThrowIfFailed(device->CreateShaderResourceView(texture.Get(), &viewDesc, &newSRV),
        "ID3D11Device::CreateShaderResourceView(texture array)");
    context->GenerateMips(newSRV.Get());

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> newSampler;
    ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &newSampler),
        "ID3D11Device::CreateSamplerState(texture array)");

    if (blockArraySampler) blockArraySampler->Release();
    if (blockArraySRV) blockArraySRV->Release();
    blockArraySampler = newSampler.Detach();
    blockArraySRV = newSRV.Detach();
}

void D3D11RenderEngine::AttachGUI(const TextureManager* textures)
{
    if (!device || !context || !state->window)
    {
        throw std::runtime_error("[ERROR] Direct3D 11 GUI setup requested before the renderer is initialized.");
    }
    GUIController::InitD3D11(state->window, device, context, state, textures);
    guiAttached = true;
}

bool D3D11RenderEngine::BeginFrame()
{
    if (state->framebufferResized.exchange(false))
    {
        int width = state->pendingFramebufferWidth.load();
        int height = state->pendingFramebufferHeight.load();
        if (width <= 0 || height <= 0) return false;
        Resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }

    if (shaderReloadRequested.exchange(false)) ReloadPassShaders();

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(state->window, &width, &height);
    const bool canRender = width > 0 && height > 0 && swapChain != nullptr && backBufferRTV != nullptr;

    // Block until the queue has room, so this frame is built from input that
    // is at most BufferCount frames old. Only when we're actually going to
    // present: each wait consumes a slot that only a Present gives back. The
    // short timeout keeps a minimized or occluded window (where frames may stop
    // completing) from stalling the loop.
    if (canRender && frameLatencyWaitable != nullptr)
    {
        WaitForSingleObjectEx(static_cast<HANDLE>(frameLatencyWaitable), 100, TRUE);
    }
    return canRender;
}

void D3D11RenderEngine::UploadCamera(const SceneView& scene)
{
    CameraUBO ubo{ scene.view, scene.proj, scene.lightingEnabled ? 1.0f : 0.0f,
        scene.brightnessFactor, scene.dayBrightness, scene.lightFalloff,
        scene.shadeTop, scene.shadeBottom, scene.shadeNorthSouth, scene.shadeEastWest,
        glm::vec4(scene.skyColor, 1.0f), glm::vec4(scene.cameraPosition, 1.0f), scene.fogStart, scene.fogEnd,
        scene.specularStrength, scene.time, glm::vec4(scene.sunDirection, 0.0f) };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT result = context->Map(cameraBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    ThrowIfFailed(result, "ID3D11DeviceContext::Map(camera)");
    std::memcpy(mapped.pData, &ubo, sizeof(ubo));
    context->Unmap(cameraBuffer, 0);
}

void D3D11RenderEngine::SubmitFrame(const SceneView& scene)
{
    if (!context || !swapChain || !backBufferRTV) return;

    // SceneView::proj carries Vulkan's Y-down clip-space flip (see
    // RenderThread). DX11 clip space is Y-up like GL, so undo it once here -
    // every pass and the camera UBO then see a projection that is correct
    // for this backend.
    SceneView d3dScene = scene;
    d3dScene.proj[1][1] *= -1.0f;

    UploadCamera(d3dScene);
    // Past this frame's start: lets SlabBuffer reuse regions freed enough
    // frames ago (see SlabBuffer::FREE_DELAY_FRAMES).
    renderFrameCounter++;

    FrameContext frame{};
    frame.engine = this;
    frame.state = state;
    frame.scene = &d3dScene;
    frame.frameIndex = 0;
    frame.extent = extent;
    frame.viewProj = d3dScene.proj * d3dScene.view;

    // Pre-main passes (shadow maps, ...) bind whatever targets they need.
    RecordPasses(frame, PassStage::PRE_MAIN);

    context->OMSetRenderTargets(1, &backBufferRTV, depthView);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(extent.width);
    viewport.Height = static_cast<float>(extent.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    const float clearColor[] = { scene.skyColor.r, scene.skyColor.g, scene.skyColor.b, 1.0f };
    context->ClearRenderTargetView(backBufferRTV, clearColor);
    if (depthView) context->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    RecordPasses(frame, PassStage::MAIN);

    // The Vulkan overlay pass keeps the color target but has no depth
    // attachment, so overlays draw on top of the finished frame untested.
    context->OMSetRenderTargets(1, &backBufferRTV, nullptr);
    RecordPasses(frame, PassStage::OVERLAY);

    if (guiAttached) GUIController::RenderD3D11(&scene);

    if (screenshotRequested.exchange(false)) CaptureScreenshot();

    const UINT presentFlags = (!vsync && tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT result = swapChain->Present(vsync ? 1 : 0, presentFlags);
    if (FAILED(result))
    {
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
        {
            throw std::runtime_error(HResultMessage("DXGI Present", device->GetDeviceRemovedReason()));
        }
        ThrowIfFailed(result, "IDXGISwapChain::Present");
    }

    // VulkanInit::NotifyFramePresented's DX11 twin: the window may already
    // have real focus by the time this first frame lands, so no focus
    // callback will fire to capture the cursor.
    if (!firstFramePresented)
    {
        firstFramePresented = true;
        if (state->windowFocused.load() && !GUIController::IsScreenOpen())
        {
            state->pendingCursorMode.store(GLFW_CURSOR_DISABLED);
        }
    }
}

void D3D11RenderEngine::CreateBackBufferResources()
{
    ID3D11Texture2D* backBuffer = nullptr;
    ThrowIfFailed(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "IDXGISwapChain::GetBuffer");
    HRESULT result = device->CreateRenderTargetView(backBuffer, nullptr, &backBufferRTV);
    D3D11_TEXTURE2D_DESC backBufferDesc{};
    backBuffer->GetDesc(&backBufferDesc);
    backBuffer->Release();
    ThrowIfFailed(result, "ID3D11Device::CreateRenderTargetView");

    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = backBufferDesc.Width;
    depthDesc.Height = backBufferDesc.Height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ThrowIfFailed(device->CreateTexture2D(&depthDesc, nullptr, &depthTexture), "ID3D11Device::CreateTexture2D(depth)");
    ThrowIfFailed(device->CreateDepthStencilView(depthTexture, nullptr, &depthView),
        "ID3D11Device::CreateDepthStencilView");
    extent = { backBufferDesc.Width, backBufferDesc.Height };
}

void D3D11RenderEngine::ReleaseBackBufferResources()
{
    if (context) context->OMSetRenderTargets(0, nullptr, nullptr);
    if (depthView)
    {
        depthView->Release();
        depthView = nullptr;
    }
    if (depthTexture)
    {
        depthTexture->Release();
        depthTexture = nullptr;
    }
    if (backBufferRTV)
    {
        backBufferRTV->Release();
        backBufferRTV = nullptr;
    }
}

void D3D11RenderEngine::Resize(uint32_t width, uint32_t height)
{
    if (!swapChain || width == 0 || height == 0) return;
    ReleaseBackBufferResources();
    if (context) context->Flush();
    // Flags must match the swapchain's creation flags.
    ThrowIfFailed(swapChain->ResizeBuffers(bufferCount, width, height, DXGI_FORMAT_UNKNOWN, SwapChainFlags()),
        "IDXGISwapChain::ResizeBuffers");
    // BufferCount may have changed (Settings), and the latency applies per buffer set.
    ConfigureFrameLatency();
    CreateBackBufferResources();
}

uint32_t D3D11RenderEngine::SwapChainFlags() const
{
    if (!flipModel) return 0; // The waitable object and tearing both need a flip model.
    return DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
        | (tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
}

void D3D11RenderEngine::ConfigureFrameLatency()
{
    if (frameLatencyWaitable != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(frameLatencyWaitable));
        frameLatencyWaitable = nullptr;
    }
    if (!swapChain || !flipModel) return;

    ComPtr<IDXGISwapChain2> swapChain2;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain2))))
    {
        Log::Info("DXGI frame latency control unavailable; using the DXGI default queue depth.");
        return;
    }
    if (SUCCEEDED(swapChain2->SetMaximumFrameLatency(bufferCount)))
    {
        // Each call returns a new handle that we own and must close.
        frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
    }
}

void D3D11RenderEngine::CaptureScreenshot()
{
    ID3D11Texture2D* backBuffer = nullptr;
    ThrowIfFailed(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "IDXGISwapChain::GetBuffer(screenshot)");

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(result))
    {
        backBuffer->Release();
        ThrowIfFailed(result, "ID3D11Device::CreateTexture2D(screenshot)");
    }

    context->CopyResource(staging, backBuffer);
    backBuffer->Release();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        staging->Release();
        ThrowIfFailed(result, "ID3D11DeviceContext::Map(screenshot)");
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
    for (uint32_t y = 0; y < desc.Height; ++y)
    {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        std::memcpy(pixels.data() + static_cast<size_t>(y) * desc.Width * 4, row,
            static_cast<size_t>(desc.Width) * 4);
    }
    context->Unmap(staging, 0);
    staging->Release();
    Screenshot::SaveAsync(std::move(pixels), desc.Width, desc.Height, "screenshots");
}

void D3D11RenderEngine::StopRendering()
{
    if (device && context) WaitIdle();
    if (guiAttached)
    {
        GUIController::Shutdown();
        guiAttached = false;
    }
    if (passesCreated) DestroyPasses();
}

void D3D11RenderEngine::Shutdown()
{
    if (initialized) StopRendering();
    // Slab buffers are ID3D11Buffers owned by the mesher statics; they must
    // be released while the device is still alive (the Vulkan twin does the
    // same in VulkanInit's cleanup).
    ChunkMesher::Shutdown();
    NonCubicMesher::Shutdown();
    ReleaseBackBufferResources();
    if (blockArraySampler)
    {
        blockArraySampler->Release();
        blockArraySampler = nullptr;
    }
    if (blockArraySRV)
    {
        blockArraySRV->Release();
        blockArraySRV = nullptr;
    }
    if (cameraBuffer)
    {
        cameraBuffer->Release();
        cameraBuffer = nullptr;
    }
    firstFramePresented = false;
    if (frameLatencyWaitable != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(frameLatencyWaitable));
        frameLatencyWaitable = nullptr;
    }
    if (swapChain)
    {
        swapChain->SetFullscreenState(FALSE, nullptr);
        swapChain->Release();
        swapChain = nullptr;
    }
    if (context)
    {
        context->ClearState();
        context->Flush();
        context->Release();
        context = nullptr;
    }
    if (device)
    {
        device->Release();
        device = nullptr;
    }
    if (state->window)
    {
        glfwDestroyWindow(state->window);
        state->window = nullptr;
    }
    if (g_windowState == state) g_windowState = nullptr;
    glfwTerminate();
    hwnd = nullptr;
    extent = {};
    initialized = false;
    guiAttached = false;
}

void D3D11RenderEngine::WaitIdle()
{
    if (!device || !context) return;
    D3D11_QUERY_DESC desc{ D3D11_QUERY_EVENT, 0 };
    ID3D11Query* query = nullptr;
    ThrowIfFailed(device->CreateQuery(&desc, &query), "ID3D11Device::CreateQuery");
    context->End(query);
    context->Flush();
    HRESULT result = S_FALSE;
    while (result == S_FALSE)
    {
        result = context->GetData(query, nullptr, 0, 0);
        if (result == S_FALSE) std::this_thread::yield();
    }
    query->Release();
    ThrowIfFailed(result, "ID3D11DeviceContext::GetData");
}

void D3D11RenderEngine::ApplyWindowMode(uint32_t mode, uint32_t requestedWidth,
    uint32_t requestedHeight, uint32_t requestedRefresh)
{
    if (!state->window) return;

    if (windowMode == 0 && mode != 0)
    {
        glfwGetWindowPos(state->window, &windowedX, &windowedY);
        int width = 0;
        int height = 0;
        glfwGetWindowSize(state->window, &width, &height);
        windowedWidth = static_cast<uint32_t>(std::max(width, 1));
        windowedHeight = static_cast<uint32_t>(std::max(height, 1));
    }

    if (mode == 0)
    {
        glfwSetWindowMonitor(state->window, nullptr, windowedX, windowedY,
            static_cast<int>(windowedWidth), static_cast<int>(windowedHeight), GLFW_DONT_CARE);
        glfwSetWindowAttrib(state->window, GLFW_DECORATED, GLFW_TRUE);
        windowMode = 0;
        return;
    }

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* desktopMode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (!monitor || !desktopMode) return;

    // Borderless: a popup window over the whole monitor, which DWM presents directly with the flip-model swap chain
    // (and which OBS sees as fullscreen). GLFW's calls alone sized it before undecorating it, which clamped it to the
    // work area; see PlaceBorderlessWindow.
    if (mode == 1 && Volcano::Platform::OS::PlaceBorderlessWindow(state->window, monitor))
    {
        windowMode = mode;
        return;
    }

    int x = 0;
    int y = 0;
    glfwGetMonitorPos(monitor, &x, &y);
    int width = desktopMode->width;
    int height = desktopMode->height;
    int refresh = GLFW_DONT_CARE;
    if (mode == 2 && requestedWidth > 0 && requestedHeight > 0)
    {
        int modeCount = 0;
        const GLFWvidmode* modes = glfwGetVideoModes(monitor, &modeCount);
        for (int i = 0; i < modeCount; ++i)
        {
            if (modes[i].width == static_cast<int>(requestedWidth)
                && modes[i].height == static_cast<int>(requestedHeight)
                && (requestedRefresh == 0 || modes[i].refreshRate == static_cast<int>(requestedRefresh)))
            {
                width = modes[i].width;
                height = modes[i].height;
                refresh = modes[i].refreshRate;
                break;
            }
        }
    }

    glfwSetWindowAttrib(state->window, GLFW_DECORATED, GLFW_FALSE);
    glfwSetWindowMonitor(state->window, mode == 2 ? monitor : nullptr, x, y, width, height, refresh);
    windowMode = mode;
}

void D3D11RenderEngine::ToggleWindowMode()
{
    uint32_t nextMode = windowMode == 0 ? 1u : 0u;
    state->config->Set("Window.Mode", nextMode);
    ApplyWindowSettings();
}

void D3D11RenderEngine::ApplyWindowSettings()
{
    if (!state->config || !state->window) return;
    uint64_t version = state->config->GetVersion();
    if (version == appliedConfigVersion) return;

    uint32_t wantedMode = std::get<uint32_t>(state->config->Get("Window.Mode", uint32_t{0}));
    windowedWidth = std::get<uint32_t>(state->config->Get("Window.Width", uint32_t{854}));
    windowedHeight = std::get<uint32_t>(state->config->Get("Window.Height", uint32_t{480}));
    fullscreenWidth = std::get<uint32_t>(state->config->Get("Window.FullscreenMode.Width", uint32_t{0}));
    fullscreenHeight = std::get<uint32_t>(state->config->Get("Window.FullscreenMode.Height", uint32_t{0}));
    refreshRate = std::get<uint32_t>(state->config->Get("Window.FullscreenMode.RefreshRate", uint32_t{0}));
    vsync = std::get<bool>(state->config->Get("Graphics.VSync", true));
    uint32_t wantedBuffers = std::clamp(
        std::get<uint32_t>(state->config->Get("Graphics.BufferCount", uint32_t{2})), 2u, 3u);
    if (wantedBuffers != bufferCount)
    {
        bufferCount = wantedBuffers;
        state->pendingFramebufferWidth = extent.width;
        state->pendingFramebufferHeight = extent.height;
        state->framebufferResized = true;
    }

    if (wantedMode != windowMode)
    {
        ApplyWindowMode(wantedMode, fullscreenWidth, fullscreenHeight, refreshRate);
    }
    else if (wantedMode == 0)
    {
        int width = 0;
        int height = 0;
        glfwGetWindowSize(state->window, &width, &height);
        if (width != static_cast<int>(windowedWidth) || height != static_cast<int>(windowedHeight))
        {
            glfwSetWindowSize(state->window, static_cast<int>(windowedWidth), static_cast<int>(windowedHeight));
        }
    }
    appliedConfigVersion = version;
}

} // namespace Volcano::Engine::D3D11
