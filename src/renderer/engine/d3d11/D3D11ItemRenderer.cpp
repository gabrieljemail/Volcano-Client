#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "D3D11ItemRenderer.hpp"
#include "D3D11ItemRendererFactory.hpp"
#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <stb_image.h>
#include "D3D11RenderEngine.hpp"
#include "D3D11Shader.hpp"
#include "passes/D3D11EntityAssets.hpp"
#include "../../TextureManager.hpp"
#include "../../misc/ItemModel.hpp"
#include "../../misc/ItemDraw.hpp"
#include "../../../Logger.hpp"
#include "../../../resources/ImageLoad.hpp"

namespace Volcano::Engine::D3D11 {

namespace {

constexpr DXGI_FORMAT ICON_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM; // Same gamma-space convention as the texture array.

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result))
    {
        throw std::runtime_error(std::string("[ERROR] D3D11ItemRenderer: ") + operation + " failed.");
    }
}

} // namespace

void D3D11ItemRenderer::Init()
{
    if (initialized) return;
    device = engine->GetD3D11Device();
    context = engine->GetImmediateContext();
    if (!device || !context) throw std::runtime_error("[ERROR] D3D11ItemRenderer: no device.");

    vertexShader = CreateVertexShader(device.Get(), "item.vert");
    pixelShader = CreatePixelShader(device.Get(), "item.frag");

    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(ItemModelVertex, position)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(ItemModelVertex, normal)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(ItemModelVertex, uv)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32_FLOAT, 0, static_cast<UINT>(offsetof(ItemModelVertex, layer)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    std::vector<char> vertexBytecode = CompileShader("item.vert");
    Check(device->CreateInputLayout(elements, 4, vertexBytecode.data(), vertexBytecode.size(), &inputLayout),
        "CreateInputLayout");

    D3D11_BUFFER_DESC constantDesc{};
    constantDesc.ByteWidth = sizeof(ItemDrawParams);
    constantDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Check(device->CreateBuffer(&constantDesc, nullptr, &constants), "CreateBuffer(constants)");

    // Not culled: extruded sprites are thin shells whose sides flip winding under
    // the display transforms (mirrored hands, negative scales), and the depth
    // test already hides the far side of an opaque mesh.
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    Check(device->CreateRasterizerState(&rasterDesc, &rasterizer), "CreateRasterizerState");

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
    Check(device->CreateDepthStencilState(&depthDesc, &depthState), "CreateDepthStencilState");

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    Check(device->CreateBlendState(&blendDesc, &blendState), "CreateBlendState");

    // The glint is optional: items just go without it if it can't be built.
    try
    {
        glintPixelShader = CreatePixelShader(device.Get(), "item_glint.frag");

        D3D11_DEPTH_STENCIL_DESC glintDepth = depthDesc;
        glintDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        glintDepth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        Check(device->CreateDepthStencilState(&glintDepth, &glintDepthState), "CreateDepthStencilState(glint)");

        D3D11_BLEND_DESC glintBlend{};
        glintBlend.RenderTarget[0].BlendEnable = TRUE;
        glintBlend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_COLOR;
        glintBlend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        glintBlend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        glintBlend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        glintBlend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        glintBlend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        glintBlend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        Check(device->CreateBlendState(&glintBlend, &glintBlendState), "CreateBlendState(glint)");
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[WARN] Item glint unavailable on Direct3D 11: ") + e.what());
        glintPixelShader.Reset();
        glintDepthState.Reset();
        glintBlendState.Reset();
    }

    D3D11_TEXTURE2D_DESC depthTextureDesc{};
    depthTextureDesc.Width = GUI_ICON_SIZE;
    depthTextureDesc.Height = GUI_ICON_SIZE;
    depthTextureDesc.MipLevels = 1;
    depthTextureDesc.ArraySize = 1;
    depthTextureDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthTextureDesc.SampleDesc.Count = 1;
    depthTextureDesc.Usage = D3D11_USAGE_DEFAULT;
    depthTextureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    Check(device->CreateTexture2D(&depthTextureDesc, nullptr, &iconDepth), "CreateTexture2D(icon depth)");
    Check(device->CreateDepthStencilView(iconDepth.Get(), nullptr, &iconDepthView), "CreateDepthStencilView(icon)");

    initialized = true;
}

void D3D11ItemRenderer::ReloadTextures()
{
    meshes.clear();
    icons.clear();
    flatIcons.clear();
}

void D3D11ItemRenderer::Shutdown()
{
    ReloadTextures();
    glintSheet.Reset();
    glintSampler.Reset();
    glintSheetTried = false;
    iconStaging.Reset();
    iconDepthView.Reset();
    iconDepth.Reset();
    glintBlendState.Reset();
    blendState.Reset();
    glintDepthState.Reset();
    depthState.Reset();
    rasterizer.Reset();
    constants.Reset();
    inputLayout.Reset();
    glintPixelShader.Reset();
    pixelShader.Reset();
    vertexShader.Reset();
    entityAssets = nullptr;
    context.Reset();
    device.Reset();
    initialized = false;
}

bool D3D11ItemRenderer::HasModel(const std::string& itemName) const
{
    return textures != nullptr && ItemModel::ForItem(itemName, *textures) != nullptr;
}

const D3D11ItemRenderer::Mesh* D3D11ItemRenderer::MeshFor(const std::string& itemName, const ItemModel& model)
{
    if (auto found = meshes.find(itemName); found != meshes.end()) return found->second.vertices ? &found->second : nullptr;

    Mesh mesh;
    const auto& vertices = model.GetVertices();
    const auto& indices = model.GetIndices();
    if (!vertices.empty() && !indices.empty())
    {
        D3D11_BUFFER_DESC desc{};
        D3D11_SUBRESOURCE_DATA data{};
        desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(ItemModelVertex));
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        data.pSysMem = vertices.data();
        bool ok = SUCCEEDED(device->CreateBuffer(&desc, &data, &mesh.vertices));
        desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        data.pSysMem = indices.data();
        ok = ok && SUCCEEDED(device->CreateBuffer(&desc, &data, &mesh.indices));
        mesh.indexCount = static_cast<uint32_t>(indices.size());
        if (!ok) mesh = Mesh{};
    }
    // A failed build is cached too (null buffers), so it isn't retried every frame.
    Mesh& stored = meshes[itemName] = std::move(mesh);
    return stored.vertices ? &stored : nullptr;
}

void D3D11ItemRenderer::Upload(const ItemDrawParams& draw)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &draw, sizeof(draw));
    context->Unmap(constants.Get(), 0);
}

void D3D11ItemRenderer::BindOpaque()
{
    context->IASetInputLayout(inputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);
    ID3D11Buffer* buffer = constants.Get();
    context->VSSetConstantBuffers(0, 1, &buffer);
    context->RSSetState(rasterizer.Get());
    context->OMSetDepthStencilState(depthState.Get(), 0);
    context->OMSetBlendState(blendState.Get(), nullptr, 0xFFFFFFFF);
    ID3D11ShaderResourceView* views[] = { engine->GetBlockTextureArray() };
    ID3D11SamplerState* samplers[] = { engine->GetBlockTextureSampler() };
    context->PSSetShaderResources(0, 1, views);
    context->PSSetSamplers(0, 1, samplers);
}

void D3D11ItemRenderer::BindGlint()
{
    context->PSSetShader(glintPixelShader.Get(), nullptr, 0);
    context->OMSetDepthStencilState(glintDepthState.Get(), 0);
    context->OMSetBlendState(glintBlendState.Get(), nullptr, 0xFFFFFFFF);
    // The glint shader takes the block array at t0 and the glint sheet at t1.
    ID3D11ShaderResourceView* views[] = { engine->GetBlockTextureArray(), glintSheet.Get() };
    ID3D11SamplerState* samplers[] = { engine->GetBlockTextureSampler(), glintSampler.Get() };
    context->PSSetShaderResources(0, 2, views);
    context->PSSetSamplers(0, 2, samplers);
}

void D3D11ItemRenderer::DrawMesh(const Mesh& mesh)
{
    UINT stride = sizeof(ItemModelVertex);
    UINT offset = 0;
    ID3D11Buffer* vertices = mesh.vertices.Get();
    context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
    context->IASetIndexBuffer(mesh.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
    context->DrawIndexed(mesh.indexCount, 0, 0);
}

bool D3D11ItemRenderer::LoadGlintSheet()
{
    if (glintSheetTried) return glintSheet != nullptr;
    glintSheetTried = true;

    int width = 0, height = 0;
    uint8_t* pixels = LoadImageRGBA("assets/minecraft/textures/misc/enchanted_glint_item.png", width, height);
    if (pixels == nullptr) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = ICON_FORMAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{ pixels, static_cast<UINT>(width) * 4, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    bool ok = SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture))
        && SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &glintSheet));
    stbi_image_free(pixels);

    if (ok)
    {
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(device->CreateSamplerState(&samplerDesc, &glintSampler));
    }
    if (!ok)
    {
        glintSheet.Reset();
        glintSampler.Reset();
    }
    return ok;
}

void D3D11ItemRenderer::DrawItem(const std::string& itemName, const std::string& context_, const glm::mat4& placement,
    const glm::mat4& viewProj, const glm::mat4& pivotRotation, float darkness, bool glint)
{
    if (!initialized || textures == nullptr) return;
    const ItemModel* model = ItemModel::ForItem(itemName, *textures);
    if (model == nullptr) return;
    const Mesh* mesh = MeshFor(itemName, *model);
    if (mesh == nullptr) return;

    Upload(ItemDraw::MakeWorld(*model, context_, placement, viewProj, pivotRotation, darkness, ItemDraw::GlintSeconds()));
    BindOpaque();
    DrawMesh(*mesh);

    if (glint && glintPixelShader && LoadGlintSheet())
    {
        BindGlint();
        DrawMesh(*mesh);
    }
}

D3D11ItemRenderer::Icon* D3D11ItemRenderer::RenderIcon(const std::string& itemName, bool glint)
{
    if (!initialized || textures == nullptr) return nullptr;
    const std::string key = itemName + (glint ? "|glint" : "");
    if (auto found = icons.find(key); found != icons.end()) return found->second.view ? &found->second : nullptr;

    const ItemModel* model = ItemModel::ForItem(itemName, *textures);
    if (model == nullptr) return nullptr;
    const Mesh* mesh = MeshFor(itemName, *model);
    if (mesh == nullptr) return nullptr;

    Icon icon;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = GUI_ICON_SIZE;
    desc.Height = GUI_ICON_SIZE;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = ICON_FORMAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bool ok = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &icon.texture))
        && SUCCEEDED(device->CreateRenderTargetView(icon.texture.Get(), nullptr, &icon.target))
        && SUCCEEDED(device->CreateShaderResourceView(icon.texture.Get(), nullptr, &icon.view));
    if (!ok)
    {
        icons[key] = Icon{}; // Cache the failure.
        return nullptr;
    }

    // This runs while the UI is being built, outside any pass: leave the
    // render target and viewport as it found them.
    ID3D11RenderTargetView* previousTarget = nullptr;
    ID3D11DepthStencilView* previousDepth = nullptr;
    context->OMGetRenderTargets(1, &previousTarget, &previousDepth);
    UINT viewportCount = 1;
    D3D11_VIEWPORT previousViewport{};
    context->RSGetViewports(&viewportCount, &previousViewport);

    ID3D11RenderTargetView* target = icon.target.Get();
    context->OMSetRenderTargets(1, &target, iconDepthView.Get());
    D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(GUI_ICON_SIZE), static_cast<float>(GUI_ICON_SIZE), 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearRenderTargetView(target, clear);
    context->ClearDepthStencilView(iconDepthView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    // No Y flip: Direct3D clip space is Y-up, and row 0 of the texture is the top.
    Upload(ItemDraw::MakeGuiIcon(*model, false));
    BindOpaque();
    DrawMesh(*mesh);
    if (glint && glintPixelShader && LoadGlintSheet())
    {
        BindGlint();
        DrawMesh(*mesh);
    }

    context->OMSetRenderTargets(1, &previousTarget, previousDepth);
    if (viewportCount > 0) context->RSSetViewports(1, &previousViewport);
    if (previousTarget != nullptr) previousTarget->Release();
    if (previousDepth != nullptr) previousDepth->Release();

    return &(icons[key] = std::move(icon));
}

ID3D11ShaderResourceView* D3D11ItemRenderer::GuiIconView(const std::string& itemName, bool glint)
{
    Icon* icon = RenderIcon(itemName, glint);
    return icon != nullptr ? icon->view.Get() : nullptr;
}

bool D3D11ItemRenderer::GuiIconPixels(const std::string& itemName, bool glint, std::vector<uint8_t>& rgba)
{
    Icon* icon = RenderIcon(itemName, glint);
    if (icon == nullptr) return false;

    if (!iconStaging)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = GUI_ICON_SIZE;
        desc.Height = GUI_ICON_SIZE;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = ICON_FORMAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &iconStaging))) return false;
    }

    context->CopyResource(iconStaging.Get(), icon->texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(iconStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    rgba.resize(static_cast<size_t>(GUI_ICON_SIZE) * GUI_ICON_SIZE * 4);
    for (uint32_t row = 0; row < GUI_ICON_SIZE; row++)
    {
        std::memcpy(rgba.data() + static_cast<size_t>(row) * GUI_ICON_SIZE * 4,
            static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
            static_cast<size_t>(GUI_ICON_SIZE) * 4);
    }
    context->Unmap(iconStaging.Get(), 0);
    return true;
}

ID3D11ShaderResourceView* D3D11ItemRenderer::FlatIconView(uint32_t layer)
{
    if (!initialized || textures == nullptr) return nullptr;
    if (auto found = flatIcons.find(layer); found != flatIcons.end()) return found->second.Get();

    const uint8_t* pixels = textures->GetLayerPixels(layer);
    const uint32_t size = textures->LayerSize();
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    if (pixels != nullptr && size != 0)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = size;
        desc.Height = size;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = ICON_FORMAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{ pixels, size * 4, 0 };
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)))
        {
            device->CreateShaderResourceView(texture.Get(), nullptr, &view);
        }
    }
    flatIcons[layer] = view; // Null is cached too.
    return view.Get();
}

D3D11ItemRenderer* CreateItemRenderer(RenderEngine* engine, const TextureManager* textures)
{
    D3D11RenderEngine* d3d = engine != nullptr ? engine->GetD3D11() : nullptr;
    if (d3d == nullptr) return nullptr;
    auto renderer = std::make_unique<D3D11ItemRenderer>(d3d, textures);
    try
    {
        renderer->Init();
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[WARN] Direct3D 11 item renderer unavailable: ") + e.what());
        return nullptr;
    }
    return renderer.release();
}

void ShutdownItemRenderer(D3D11ItemRenderer* renderer)
{
    if (renderer != nullptr) renderer->Shutdown();
}

void DestroyItemRenderer(D3D11ItemRenderer* renderer)
{
    delete renderer;
}

} // namespace Volcano::Engine::D3D11
