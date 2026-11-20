#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <wrl/client.h>
#include <stb_image.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "D3D11SkyboxPass.hpp"
#include "../D3D11RenderEngine.hpp"
#include "../D3D11Shader.hpp"
#include "../../../../Logger.hpp"
#include "../../../../resources/ImageLoad.hpp"

namespace Volcano::Engine::D3D11 {

namespace {

constexpr const char* PANORAMA_DIR = "assets/minecraft/textures/gui/title/background/"; // Logical pack paths.
constexpr const char* CELESTIAL_DIR = "assets/minecraft/textures/environment/celestial/";

// Moon phases in vanilla order (phase 0 = full moon).
constexpr std::array<const char*, 8> MOON_PHASES = {
    "full_moon", "waning_gibbous", "third_quarter", "waning_crescent",
    "new_moon", "waxing_crescent", "first_quarter", "waxing_gibbous" };

// Cube layer (+X,-X,+Y,-Y,+Z,-Z) -> panorama_N.png, from vanilla's
// CubeMapTexture.SUFFIXES.
constexpr std::array<int, 6> FACE_FILE = { 1, 3, 5, 4, 0, 2 };

// CubeMap.PROJECTION_*, and the fixed downward tilt it renders with.
constexpr float FOV_DEGREES = 85.0f;
constexpr float NEAR_PLANE = 0.05f;
constexpr float FAR_PLANE = 10.0f;
constexpr float PITCH_DEGREES = 10.0f;

// Vanilla's Panorama adds deltaTicks * panoramaSpeed(1.0) * 0.1 degrees per
// tick, at 20 ticks a second.
constexpr float SPIN_DEGREES_PER_SECOND = 2.0f;

// Must match skybox.frag's Push block (cbuffer at b0).
struct Push {
    glm::mat4 mvp;
    glm::vec4 skyTop;   // w = mode (0 panorama, 1 sky)
    glm::vec4 horizon;  // w = moon phase
    glm::vec4 sunDir;
};

} // namespace

D3D11SkyboxPass::D3D11SkyboxPass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
    : RenderPass(NAME), device(d3dDevice), context(d3dContext) {}

void D3D11SkyboxPass::Create(const ShaderLibrary& /*shaders*/)
{
    if (!LoadCubemap() || !LoadCelestial())
    {
        Destroy();
        return;
    }

    try
    {
        CreatePipeline();
    }
    catch (...)
    {
        Destroy();
        throw;
    }
    Log::Info("Skybox pass active.");
}

void D3D11SkyboxPass::ReloadShaders(const ShaderLibrary& /*shaders*/)
{
    vertexShader.Reset();
    pixelShader.Reset();
    pushBuffer.Reset();
    depthState.Reset();
    rasterizer.Reset();
    CreatePipeline();
}

void D3D11SkyboxPass::ReloadTextures(const ShaderLibrary& shaders)
{
    Destroy();
    Create(shaders);
}

bool D3D11SkyboxPass::UploadLayers(const std::vector<const uint8_t*>& layers, uint32_t size, bool cube,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv)
{
    const uint32_t layerCount = static_cast<uint32_t>(layers.size());

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size;
    desc.Height = size;
    desc.MipLevels = 1;
    desc.ArraySize = layerCount;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> newTexture;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &newTexture))) return false;

    for (uint32_t layer = 0; layer < layerCount; layer++)
    {
        context->UpdateSubresource(newTexture.Get(), D3D11CalcSubresource(0, layer, 1),
            nullptr, layers[layer], size * 4, size * size * 4);
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = desc.Format;
    if (cube)
    {
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        viewDesc.TextureCube.MostDetailedMip = 0;
        viewDesc.TextureCube.MipLevels = 1;
    }
    else
    {
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.MostDetailedMip = 0;
        viewDesc.Texture2DArray.MipLevels = 1;
        viewDesc.Texture2DArray.FirstArraySlice = 0;
        viewDesc.Texture2DArray.ArraySize = layerCount;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSRV;
    if (FAILED(device->CreateShaderResourceView(newTexture.Get(), &viewDesc, &newSRV))) return false;

    texture = std::move(newTexture);
    srv = std::move(newSRV);
    return true;
}

bool D3D11SkyboxPass::LoadCubemap()
{
    std::array<stbi_uc*, 6> faces{};
    int width = 0, height = 0;
    bool ok = true;

    for (int layer = 0; layer < 6 && ok; layer++)
    {
        std::string path = std::string(PANORAMA_DIR) + "panorama_" + std::to_string(FACE_FILE[layer]) + ".png";
        int w = 0, h = 0;
        faces[layer] = LoadImageRGBA(path, w, h);
        if (!faces[layer])
        {
            Log::Error("[ERROR] Skybox: failed to load " + path);
            ok = false;
        }
        else if (layer == 0)
        {
            width = w;
            height = h;
        }
        else if (w != width || h != height)
        {
            Log::Error("[ERROR] Skybox: panorama face sizes differ (" + path + ").");
            ok = false;
        }
    }
    if (ok && width != height)
    {
        Log::Error("[ERROR] Skybox: panorama faces must be square.");
        ok = false;
    }

    // Without a usable panorama, a flat 1x1 cube keeps the in-game sky working.
    static const uint8_t FALLBACK_PIXEL[4] = { 40, 40, 48, 255 };
    std::vector<std::vector<uint8_t>> flipped;
    std::vector<const uint8_t*> layers;
    uint32_t size = 1;
    if (ok)
    {
        // Rows are copied bottom-up, matching vanilla's copyRect(..., flipY = true).
        size = static_cast<uint32_t>(width);
        const size_t rowBytes = static_cast<size_t>(width) * 4;
        for (int layer = 0; layer < 6; layer++)
        {
            std::vector<uint8_t> face(rowBytes * height);
            for (int row = 0; row < height; row++)
            {
                std::memcpy(face.data() + row * rowBytes, faces[layer] + static_cast<size_t>(height - 1 - row) * rowBytes, rowBytes);
            }
            flipped.push_back(std::move(face));
        }
        for (const auto& face : flipped) layers.push_back(face.data());
    }
    else
    {
        layers.assign(6, FALLBACK_PIXEL);
    }
    for (stbi_uc* face : faces) if (face) stbi_image_free(face);

    if (!UploadLayers(layers, size, true, cubeTexture, cubeSRV))
    {
        Log::Error("[ERROR] Skybox: failed to create the cubemap.");
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samplerDesc, &cubeSampler)))
    {
        Log::Error("[ERROR] Skybox: failed to create the cubemap sampler.");
        return false;
    }
    return true;
}

bool D3D11SkyboxPass::LoadCelestial()
{
    std::vector<std::string> paths{ std::string(CELESTIAL_DIR) + "sun.png" };
    for (const char* phase : MOON_PHASES) paths.push_back(std::string(CELESTIAL_DIR) + "moon/" + phase + ".png");

    // A missing texture becomes a transparent layer rather than failing the
    // sky. Every layer shares one size: the largest square texture (32 for
    // vanilla, more for high-resolution packs), smaller ones scaled up nearest.
    std::vector<stbi_uc*> images(paths.size(), nullptr);
    std::vector<int> widths(paths.size(), 0);
    uint32_t SIZE = 32;
    for (size_t i = 0; i < paths.size(); i++)
    {
        int w = 0, h = 0;
        images[i] = LoadImageRGBA(paths[i], w, h);
        if (images[i] && w == h && w > 0)
        {
            widths[i] = w;
            if (static_cast<uint32_t>(w) > SIZE) SIZE = static_cast<uint32_t>(w);
        }
        else
        {
            Log::Error("[ERROR] Skybox: couldn't use " + paths[i] + " (missing, or not square).");
        }
    }

    std::vector<std::vector<uint8_t>> pixels(paths.size(), std::vector<uint8_t>(static_cast<size_t>(SIZE) * SIZE * 4, 0));
    for (size_t i = 0; i < paths.size(); i++)
    {
        if (images[i] && widths[i] > 0)
        {
            const uint32_t srcSize = static_cast<uint32_t>(widths[i]);
            for (uint32_t y = 0; y < SIZE; y++)
            {
                for (uint32_t x = 0; x < SIZE; x++)
                {
                    const uint8_t* src = images[i] + (static_cast<size_t>(y * srcSize / SIZE) * srcSize + x * srcSize / SIZE) * 4;
                    std::memcpy(pixels[i].data() + (static_cast<size_t>(y) * SIZE + x) * 4, src, 4);
                }
            }
        }
        if (images[i]) stbi_image_free(images[i]);
    }

    std::vector<const uint8_t*> layers;
    for (const auto& layer : pixels) layers.push_back(layer.data());
    if (!UploadLayers(layers, SIZE, false, celestialTexture, celestialSRV))
    {
        Log::Error("[ERROR] Skybox: failed to create the sun/moon texture.");
        return false;
    }

    // Nearest: pixel art.
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samplerDesc, &celestialSampler)))
    {
        Log::Error("[ERROR] Skybox: failed to create the sun/moon sampler.");
        return false;
    }
    return true;
}

void D3D11SkyboxPass::CreatePipeline()
{
    vertexShader = CreateVertexShader(device, VERTEX_SHADER);
    pixelShader = CreatePixelShader(device, FRAGMENT_SHADER);

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(Push);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, &pushBuffer)))
    {
        throw std::runtime_error("[ERROR] Failed to create the skybox constant buffer.");
    }

    // Background only: everything after it draws over it, so no depth work.
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &depthState)))
    {
        throw std::runtime_error("[ERROR] Failed to create the skybox depth state.");
    }

    // The cube is seen from inside: no face culling.
    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizerDesc, &rasterizer)))
    {
        throw std::runtime_error("[ERROR] Failed to create the skybox rasterizer state.");
    }
}

void D3D11SkyboxPass::Record(FrameContext& frame)
{
    if (!vertexShader || !pixelShader || !pushBuffer || !cubeSRV || !celestialSRV) return;
    const SceneView& scene = *frame.scene;

    Push push{};
    if (scene.menuBackground)
    {
        // SceneView::proj arrives with Vulkan's Y-down flip already undone by
        // the engine; this local projection never had it, so no flip here.
        float aspect = static_cast<float>(frame.extent.width)
            / static_cast<float>(frame.extent.height > 0 ? frame.extent.height : 1);
        glm::mat4 proj = glm::perspective(glm::radians(FOV_DEGREES), aspect, NEAR_PLANE, FAR_PLANE);

        // CubeMap.render: rotationX(pi) . rotateX(pitch) . rotateY(-spin).
        float spin = std::fmod(scene.time * SPIN_DEGREES_PER_SECOND, 360.0f);
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(1, 0, 0));
        model = glm::rotate(model, glm::radians(PITCH_DEGREES), glm::vec3(1, 0, 0));
        model = glm::rotate(model, glm::radians(-spin), glm::vec3(0, 1, 0));
        push.mvp = proj * model;
        push.skyTop.w = 0.0f;
    }
    else
    {
        // The cube's corners are world-space directions: the camera's
        // rotation only, so the sky never moves with the player.
        push.mvp = scene.proj * glm::mat4(glm::mat3(scene.view));
        push.skyTop = glm::vec4(scene.skyTopColor, 1.0f);
        push.horizon = glm::vec4(scene.skyColor, static_cast<float>(scene.moonPhase));
        push.sunDir = glm::vec4(scene.sunDirection, 0.0f);
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(pushBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &push, sizeof(push));
    context->Unmap(pushBuffer.Get(), 0);

    ID3D11ShaderResourceView* srvs[2] = { cubeSRV.Get(), celestialSRV.Get() };
    ID3D11SamplerState* samplers[2] = { cubeSampler.Get(), celestialSampler.Get() };
    ID3D11Buffer* buffers[1] = { pushBuffer.Get() };

    context->IASetInputLayout(nullptr);
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, buffers);
    context->PSSetConstantBuffers(0, 1, buffers);
    context->PSSetShaderResources(0, 2, srvs);
    context->PSSetSamplers(0, 2, samplers);
    context->RSSetState(rasterizer.Get());
    context->OMSetDepthStencilState(depthState.Get(), 0);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    // 36 vertices of a unit cube, all generated from SV_VertexID.
    context->Draw(36, 0);
}

void D3D11SkyboxPass::Destroy()
{
    vertexShader.Reset();
    pixelShader.Reset();
    pushBuffer.Reset();
    depthState.Reset();
    rasterizer.Reset();
    cubeSRV.Reset();
    cubeTexture.Reset();
    cubeSampler.Reset();
    celestialSRV.Reset();
    celestialTexture.Reset();
    celestialSampler.Reset();
}

} // namespace Volcano::Engine::D3D11
