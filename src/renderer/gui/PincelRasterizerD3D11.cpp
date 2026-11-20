#include "PincelRasterizer.hpp"

#ifdef _WIN32

#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "../../Logger.hpp"
#include "../engine/d3d11/D3D11RenderEngine.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace Volcano::GUI {

namespace {

struct PincelConstants {
    float viewport[4]{};
    float rect[4]{};
    float borders[4]{};
    float borderColor[4]{};
    float radius = 0.0f;
    int32_t kind = 0;
    float padding[2]{};
};

static_assert(sizeof(PincelConstants) == 80);

struct D3DTexture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

std::string HResultMessage(const char* operation, HRESULT result)
{
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
    return std::string(operation) + " failed (" + code + ").";
}

void ThrowIfFailed(HRESULT result, const char* operation)
{
    if (FAILED(result)) throw std::runtime_error(HResultMessage(operation, result));
}

ComPtr<ID3DBlob> CompileShader(const wchar_t* path, const char* profile)
{
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    HRESULT result = D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
    if (FAILED(result))
    {
        std::string message = HResultMessage("D3DCompileFromFile", result);
        if (errors != nullptr)
        {
            message += " ";
            message.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }
    return shader;
}

bool CreateTexture(ID3D11Device* device, D3DTexture& destination, uint32_t width, uint32_t height,
    DXGI_FORMAT format, const uint8_t* pixels, size_t byteCount)
{
    if (device == nullptr || pixels == nullptr || width == 0 || height == 0) return false;
    uint32_t bytesPerPixel = format == DXGI_FORMAT_R8_UNORM ? 1u : 4u;
    if (width > std::numeric_limits<UINT>::max() / bytesPerPixel
        || static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / bytesPerPixel / height
        || byteCount != static_cast<size_t>(width) * height * bytesPerPixel)
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = pixels;
    initialData.SysMemPitch = width * bytesPerPixel;

    D3DTexture texture;
    HRESULT result = device->CreateTexture2D(&desc, &initialData, &texture.texture);
    if (FAILED(result))
    {
        Log::Error("[ERROR] " + HResultMessage("ID3D11Device::CreateTexture2D(Pincel)", result));
        return false;
    }
    result = device->CreateShaderResourceView(texture.texture.Get(), nullptr, &texture.view);
    if (FAILED(result))
    {
        Log::Error("[ERROR] " + HResultMessage("ID3D11Device::CreateShaderResourceView(Pincel)", result));
        return false;
    }
    texture.width = width;
    texture.height = height;
    texture.format = format;
    destination = std::move(texture);
    return true;
}

} // namespace

class D3D11PincelRasterizer final : public PincelRasterizer {
public:
    bool Create(Engine::RenderEngine& engine) override
    {
        renderer = engine.GetD3D11();
        if (renderer == nullptr || renderer->GetD3D11Device() == nullptr) return false;
        device = renderer->GetD3D11Device();

        try
        {
            ComPtr<ID3DBlob> vertexCode = CompileShader(L"resources\\shaders\\pincel_draw.vert.hlsl", "vs_4_0");
            ComPtr<ID3DBlob> pixelCode = CompileShader(L"resources\\shaders\\pincel_draw.frag.hlsl", "ps_4_0");
            ThrowIfFailed(device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(),
                nullptr, &vertexShader), "ID3D11Device::CreateVertexShader(Pincel)");
            ThrowIfFailed(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(),
                nullptr, &pixelShader), "ID3D11Device::CreatePixelShader(Pincel)");

            const D3D11_INPUT_ELEMENT_DESC elements[] = {
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Pincel::DrawVertex, x),
                    D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Pincel::DrawVertex, u),
                    D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                    offsetof(Pincel::DrawVertex, color) + offsetof(Pincel::DrawColor, r),
                    D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            ThrowIfFailed(device->CreateInputLayout(elements, static_cast<UINT>(std::size(elements)),
                vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), &inputLayout),
                "ID3D11Device::CreateInputLayout(Pincel)");

            D3D11_BUFFER_DESC constantDesc{};
            constantDesc.ByteWidth = sizeof(PincelConstants);
            constantDesc.Usage = D3D11_USAGE_DYNAMIC;
            constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ThrowIfFailed(device->CreateBuffer(&constantDesc, nullptr, &constantBuffer),
                "ID3D11Device::CreateBuffer(Pincel constants)");

            D3D11_BLEND_DESC blendDesc{};
            blendDesc.RenderTarget[0].BlendEnable = TRUE;
            blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ThrowIfFailed(device->CreateBlendState(&blendDesc, &blendState),
                "ID3D11Device::CreateBlendState(Pincel)");

            D3D11_RASTERIZER_DESC rasterDesc{};
            rasterDesc.FillMode = D3D11_FILL_SOLID;
            rasterDesc.CullMode = D3D11_CULL_NONE;
            rasterDesc.DepthClipEnable = TRUE;
            rasterDesc.ScissorEnable = TRUE;
            ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, &rasterizerState),
                "ID3D11Device::CreateRasterizerState(Pincel)");

            D3D11_DEPTH_STENCIL_DESC depthDesc{};
            depthDesc.DepthEnable = FALSE;
            depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            depthDesc.StencilEnable = FALSE;
            ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, &depthStencilState),
                "ID3D11Device::CreateDepthStencilState(Pincel)");

            for (size_t i = 0; i < samplers.size(); ++i)
            {
                D3D11_SAMPLER_DESC samplerDesc{};
                samplerDesc.Filter = i == 0 ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
                samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
                samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
                ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &samplers[i]),
                    "ID3D11Device::CreateSamplerState(Pincel)");
            }

            static constexpr uint8_t whitePixel[] = { 255, 255, 255, 255 };
            if (!CreateTexture(device.Get(), fallbackTexture, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                    whitePixel, sizeof(whitePixel)))
            {
                throw std::runtime_error("failed to create the fallback texture");
            }
        }
        catch (const std::exception& error)
        {
            Log::Error(std::string("[ERROR] PincelRasterizer: ") + error.what());
            Destroy();
            return false;
        }
        return true;
    }

    void Destroy() override
    {
        imageTextures.clear();
        fontTexture = {};
        fallbackTexture = {};
        for (auto& frame : frames)
        {
            frame.vertexBuffer.Reset();
            frame.indexBuffer.Reset();
            frame.vertexCapacity = 0;
            frame.indexCapacity = 0;
            frame = {};
        }
        constantBuffer.Reset();
        inputLayout.Reset();
        vertexShader.Reset();
        pixelShader.Reset();
        blendState.Reset();
        rasterizerState.Reset();
        depthStencilState.Reset();
        for (auto& sampler : samplers) sampler.Reset();
        device.Reset();
        renderer = nullptr;
        uploadedVersion = 0;
        currentFrame = 0;
    }

    void Upload(const Pincel::DrawData& data) override
    {
        if (device == nullptr) return;
        FrameBuffers& frame = frames[currentFrame];

        static constexpr uint8_t whitePixel[] = { 255, 255, 255, 255 };
        if (fallbackTexture.view == nullptr
            && !CreateTexture(device.Get(), fallbackTexture, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                whitePixel, sizeof(whitePixel)))
        {
            Log::Error("[ERROR] PincelRasterizer: failed to create the fallback texture");
            return;
        }

        std::unordered_set<uint64_t> liveImages;
        for (const Pincel::ImageTextureView& source : data.images)
        {
            if (source.id == 0 || source.pixels == nullptr || source.width == 0 || source.height == 0)
            {
                Log::Error("[ERROR] PincelRasterizer: received an invalid image texture");
                continue;
            }
            liveImages.insert(source.id);
            D3DTexture& texture = imageTextures[source.id];
            if (texture.view == nullptr)
            {
                CreateTexture(device.Get(), texture, source.width, source.height, DXGI_FORMAT_R8G8B8A8_UNORM,
                    source.pixels, source.byteCount);
            }
            else if (texture.width != source.width || texture.height != source.height)
            {
                Log::Error("[ERROR] PincelRasterizer: an image id changed dimensions");
            }
        }
        for (auto it = imageTextures.begin(); it != imageTextures.end();)
        {
            if (!liveImages.contains(it->first)) it = imageTextures.erase(it);
            else ++it;
        }

        if (data.fontAtlas.pixels != nullptr && data.fontAtlas.width > 0 && data.fontAtlas.height > 0
            && (fontTexture.view == nullptr || fontTexture.width != data.fontAtlas.width
                || fontTexture.height != data.fontAtlas.height || fontTextureVersion != data.fontAtlas.version))
        {
            D3DTexture newFont;
            size_t byteCount = static_cast<size_t>(data.fontAtlas.width) * data.fontAtlas.height;
            if (CreateTexture(device.Get(), newFont, data.fontAtlas.width, data.fontAtlas.height,
                    DXGI_FORMAT_R8_UNORM, data.fontAtlas.pixels, byteCount))
            {
                fontTexture = std::move(newFont);
                fontTextureVersion = data.fontAtlas.version;
            }
        }

        if (data.vertices.size() > std::numeric_limits<uint32_t>::max() - 4
            || data.indices.size() > std::numeric_limits<uint32_t>::max() - 6)
        {
            Log::Error("[ERROR] PincelRasterizer: draw data exceeds the 32-bit index limit");
            return;
        }

        std::vector<Pincel::DrawVertex> vertices = data.vertices;
        std::vector<uint32_t> indices = data.indices;
        for (Pincel::DrawVertex& vertex : vertices)
        {
            vertex.y = static_cast<float>(data.height) - vertex.y;
        }
        frame.backgroundIndexCount = 0;
        if (data.background.a > 0.0f && data.width > 0 && data.height > 0)
        {
            uint32_t base = static_cast<uint32_t>(vertices.size());
            vertices.insert(vertices.end(), {
                { 0.0f, static_cast<float>(data.height), 0.0f, 0.0f, data.background },
                { static_cast<float>(data.width), static_cast<float>(data.height), 1.0f, 0.0f, data.background },
                { static_cast<float>(data.width), 0.0f, 1.0f, 1.0f, data.background },
                { 0.0f, 0.0f, 0.0f, 1.0f, data.background },
            });
            frame.backgroundFirstIndex = static_cast<uint32_t>(indices.size());
            indices.insert(indices.end(), { base, base + 1, base + 2, base + 2, base + 3, base });
            frame.backgroundIndexCount = 6;
        }

        if (!UploadBuffer(frame.vertexBuffer, frame.vertexCapacity, vertices.data(),
                vertices.size() * sizeof(Pincel::DrawVertex), D3D11_BIND_VERTEX_BUFFER)
            || !UploadBuffer(frame.indexBuffer, frame.indexCapacity, indices.data(),
                indices.size() * sizeof(uint32_t), D3D11_BIND_INDEX_BUFFER))
        {
            return;
        }
        frame.indexCount = static_cast<uint32_t>(indices.size());
        frame.canvasWidth = data.width;
        frame.canvasHeight = data.height;
        frame.commands = data.commands;
        ++uploadedVersion;
    }

    void Record(Engine::FrameContext& frameContext) override
    {
        static bool loggedOnce = false;
        auto earlyOut = [&](const char* why) {
            if (!loggedOnce) { loggedOnce = true; Log::Info(std::string("[DX11PINCEL] Record skipped: ") + why); }
        };
        if (renderer == nullptr || frameContext.engine == nullptr
            || frameContext.engine->GetD3D11() != renderer || uploadedVersion == 0)
        {
            earlyOut(renderer == nullptr ? "no renderer" : frameContext.engine == nullptr ? "no engine"
                : frameContext.engine->GetD3D11() != renderer ? "engine mismatch" : "uploadedVersion==0");
            return;
        }
        ID3D11DeviceContext* context = renderer->GetImmediateContext();
        if (context == nullptr) { earlyOut("no context"); return; }

        const FrameBuffers& frame = frames[currentFrame];
        if (frame.vertexBuffer == nullptr || frame.indexBuffer == nullptr || frame.indexCount == 0
            || frame.canvasWidth == 0 || frame.canvasHeight == 0)
        {
            earlyOut("empty frame buffers");
            currentFrame = (currentFrame + 1) % FRAME_COUNT;
            return;
        }

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(frame.canvasWidth);
        viewport.Height = static_cast<float>(frame.canvasHeight);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        D3D11_RECT fullScissor{ 0, 0, static_cast<LONG>(frame.canvasWidth), static_cast<LONG>(frame.canvasHeight) };
        context->RSSetScissorRects(1, &fullScissor);
        context->RSSetState(rasterizerState.Get());
        context->OMSetBlendState(blendState.Get(), nullptr, 0xffffffff);
        context->OMSetDepthStencilState(depthStencilState.Get(), 0);

        UINT stride = sizeof(Pincel::DrawVertex);
        UINT offset = 0;
        ID3D11Buffer* vertexBuffer = frame.vertexBuffer.Get();
        context->IASetInputLayout(inputLayout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context->IASetIndexBuffer(frame.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);
        ID3D11Buffer* constants = constantBuffer.Get();
        context->VSSetConstantBuffers(0, 1, &constants);
        context->PSSetConstantBuffers(0, 1, &constants);

        auto draw = [&](uint32_t firstIndex, uint32_t indexCount, const Pincel::DrawCommand* command,
            ID3D11ShaderResourceView* texture, uint32_t samplerIndex)
        {
            if (indexCount == 0 || texture == nullptr || indexCount > frame.indexCount
                || firstIndex > frame.indexCount - indexCount)
            {
                return;
            }

            D3D11_RECT scissor = fullScissor;
            if (command != nullptr && command->hasClip)
            {
                int64_t left = std::max<int64_t>(0, command->clip.x);
                int64_t top = std::max<int64_t>(0, command->clip.y);
                int64_t right = std::min<int64_t>(frame.canvasWidth,
                    static_cast<int64_t>(command->clip.x) + std::max(command->clip.width, 0));
                int64_t bottom = std::min<int64_t>(frame.canvasHeight,
                    static_cast<int64_t>(command->clip.y) + std::max(command->clip.height, 0));
                if (right <= left || bottom <= top) return;
                scissor = { static_cast<LONG>(left), static_cast<LONG>(top),
                    static_cast<LONG>(right), static_cast<LONG>(bottom) };
            }
            context->RSSetScissorRects(1, &scissor);

            PincelConstants constantsData{};
            constantsData.viewport[0] = static_cast<float>(frame.canvasWidth);
            constantsData.viewport[1] = static_cast<float>(frame.canvasHeight);
            if (command != nullptr)
            {
                constantsData.rect[0] = static_cast<float>(command->rect.x);
                constantsData.rect[1] = static_cast<float>(command->rect.y);
                constantsData.rect[2] = static_cast<float>(command->rect.width);
                constantsData.rect[3] = static_cast<float>(command->rect.height);
                for (size_t i = 0; i < command->border.size(); ++i)
                    constantsData.borders[i] = static_cast<float>(command->border[i]);
                constantsData.borderColor[0] = command->borderColor.r;
                constantsData.borderColor[1] = command->borderColor.g;
                constantsData.borderColor[2] = command->borderColor.b;
                constantsData.borderColor[3] = command->borderColor.a;
                constantsData.radius = static_cast<float>(command->radius);
                constantsData.kind = command->kind == Pincel::DrawCommandKind::Box ? 0
                    : command->kind == Pincel::DrawCommandKind::Image ? 1 : 2;
            }
            else
            {
                constantsData.rect[2] = static_cast<float>(frame.canvasWidth);
                constantsData.rect[3] = static_cast<float>(frame.canvasHeight);
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                Log::Error("[ERROR] PincelRasterizer: failed to map its constants buffer");
                return;
            }
            std::memcpy(mapped.pData, &constantsData, sizeof(constantsData));
            context->Unmap(constantBuffer.Get(), 0);

            ID3D11ShaderResourceView* views[] = { texture };
            ID3D11SamplerState* sampler = samplers[samplerIndex].Get();
            context->PSSetShaderResources(0, 1, views);
            context->PSSetSamplers(0, 1, &sampler);
            context->DrawIndexed(indexCount, firstIndex, 0);
        };

        if (frame.backgroundIndexCount != 0)
        {
            draw(frame.backgroundFirstIndex, frame.backgroundIndexCount, nullptr, fallbackTexture.view.Get(), 0);
        }
        for (const Pincel::DrawCommand& command : frame.commands)
        {
            ID3D11ShaderResourceView* texture = nullptr;
            uint32_t samplerIndex = 0;
            if (command.kind == Pincel::DrawCommandKind::Box)
            {
                texture = fallbackTexture.view.Get();
            }
            else if (command.kind == Pincel::DrawCommandKind::Glyph)
            {
                texture = fontTexture.view.Get();
            }
            else if (command.kind == Pincel::DrawCommandKind::Image)
            {
                auto found = imageTextures.find(command.imageId);
                if (found != imageTextures.end()) texture = found->second.view.Get();
                samplerIndex = command.imageFilter == 1 ? 0u : 1u;
            }

            if (command.firstIndex > std::numeric_limits<uint32_t>::max()
                || command.indexCount > std::numeric_limits<uint32_t>::max())
            {
                Log::Error("[ERROR] PincelRasterizer: draw command index range exceeds 32 bits");
                continue;
            }
            draw(static_cast<uint32_t>(command.firstIndex), static_cast<uint32_t>(command.indexCount),
                &command, texture, samplerIndex);
        }
        ID3D11ShaderResourceView* noTexture = nullptr;
        context->PSSetShaderResources(0, 1, &noTexture);
        currentFrame = (currentFrame + 1) % FRAME_COUNT;
    }

    uint64_t UploadedVersion() const override { return uploadedVersion; }

private:
    struct FrameBuffers {
        ComPtr<ID3D11Buffer> vertexBuffer;
        ComPtr<ID3D11Buffer> indexBuffer;
        size_t vertexCapacity = 0;
        size_t indexCapacity = 0;
        uint32_t indexCount = 0;
        uint32_t backgroundFirstIndex = 0;
        uint32_t backgroundIndexCount = 0;
        uint32_t canvasWidth = 0;
        uint32_t canvasHeight = 0;
        std::vector<Pincel::DrawCommand> commands;
    };

    static constexpr uint32_t FRAME_COUNT = 1;

    bool UploadBuffer(ComPtr<ID3D11Buffer>& buffer, size_t& capacity, const void* source,
        size_t byteCount, UINT bindFlags)
    {
        if (byteCount == 0) return true;
        if (byteCount > std::numeric_limits<UINT>::max())
        {
            Log::Error("[ERROR] PincelRasterizer: draw buffer exceeds the Direct3D 11 size limit");
            return false;
        }
        if (buffer == nullptr || capacity < byteCount)
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(byteCount);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = bindFlags;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            HRESULT result = device->CreateBuffer(&desc, nullptr, &buffer);
            if (FAILED(result))
            {
                Log::Error("[ERROR] " + HResultMessage("ID3D11Device::CreateBuffer(Pincel draw data)", result));
                return false;
            }
            capacity = byteCount;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ID3D11DeviceContext* context = renderer->GetImmediateContext();
        HRESULT result = context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result))
        {
            Log::Error("[ERROR] " + HResultMessage("ID3D11DeviceContext::Map(Pincel draw data)", result));
            return false;
        }
        std::memcpy(mapped.pData, source, byteCount);
        context->Unmap(buffer.Get(), 0);
        return true;
    }

    Engine::D3D11::D3D11RenderEngine* renderer = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> constantBuffer;
    ComPtr<ID3D11BlendState> blendState;
    ComPtr<ID3D11RasterizerState> rasterizerState;
    ComPtr<ID3D11DepthStencilState> depthStencilState;
    std::array<ComPtr<ID3D11SamplerState>, 2> samplers;
    std::array<FrameBuffers, FRAME_COUNT> frames;
    uint32_t currentFrame = 0;
    D3DTexture fallbackTexture;
    D3DTexture fontTexture;
    std::unordered_map<uint64_t, D3DTexture> imageTextures;
    uint64_t fontTextureVersion = 0;
    uint64_t uploadedVersion = 0;
};

std::unique_ptr<PincelRasterizer> CreateD3D11PincelRasterizer()
{
    return std::make_unique<D3D11PincelRasterizer>();
}

} // namespace Volcano::GUI

#endif
