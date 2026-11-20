#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "D3D11CloudPass.hpp"
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include "../D3D11Shader.hpp"
#include "../../../../ApplicationDeps.hpp"
import Volcano;
#include "../../../../Logger.hpp"

namespace Volcano::Engine::D3D11 {

namespace {

void Check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string("[ERROR] D3D11CloudPass: ") + what + " failed.");
}

} // namespace

void D3D11CloudPass::Create(const ShaderLibrary&)
{
    // Clouds are scenery: if anything here fails the world still renders without them.
    try
    {
        if (!scene.LoadCells()) return; // Logged: the pack has no usable clouds.png.

        vertexShader = CreateVertexShader(device, VERTEX_SHADER);
        pixelShader = CreatePixelShader(device, FRAGMENT_SHADER);

        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(CloudScene::Vertex, x)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, static_cast<UINT>(offsetof(CloudScene::Vertex, shade)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        std::vector<char> bytecode = CompileShader(VERTEX_SHADER);
        Check(device->CreateInputLayout(elements, 2, bytecode.data(), bytecode.size(), &inputLayout), "CreateInputLayout");

        D3D11_BUFFER_DESC vertexDesc{};
        vertexDesc.ByteWidth = static_cast<UINT>(sizeof(CloudScene::Vertex) * CloudScene::MAX_VERTICES);
        vertexDesc.Usage = D3D11_USAGE_DYNAMIC;
        vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vertexDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&vertexDesc, nullptr, &vertexBuffer), "CreateBuffer(vertices)");

        D3D11_BUFFER_DESC constantDesc{};
        constantDesc.ByteWidth = (sizeof(CloudScene::Push) + 15u) & ~15u;
        constantDesc.Usage = D3D11_USAGE_DYNAMIC;
        constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&constantDesc, nullptr, &constants), "CreateBuffer(constants)");

        // Back faces culled with counter-clockwise front, as the terrain's (the grid is wound like it).
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_BACK;
        raster.FrontCounterClockwise = TRUE;
        raster.DepthClipEnable = TRUE;
        Check(device->CreateRasterizerState(&raster, &rasterizer), "CreateRasterizerState");

        // Behind terrain and entities, never writing depth (a cloud must not occlude what is drawn after it).
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        Check(device->CreateDepthStencilState(&depth, &depthState), "CreateDepthStencilState");

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        Check(device->CreateBlendState(&blend, &blendState), "CreateBlendState");

        scratch.resize(CloudScene::MAX_VERTICES);
        Log::Info("Cloud pass active (Direct3D 11).");
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[WARN] Clouds unavailable on Direct3D 11: ") + e.what());
        Destroy();
    }
}

void D3D11CloudPass::Record(FrameContext& frame)
{
    if (!vertexBuffer || frame.state == nullptr) return;

    // Where the grid is, its style, color and transform: shared with the Vulkan pass.
    CloudScene::Draw draw;
    if (!scene.Plan(*frame.scene, frame.state->config, draw)) return;

    if (mode != draw.mode || centerX != draw.centerX || centerZ != draw.centerZ)
    {
        vertexCount = scene.BuildMesh(scratch.data(), draw.centerX, draw.centerZ, draw.mode);
        centerX = draw.centerX;
        centerZ = draw.centerZ;
        mode = draw.mode;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            mode = -1; // Try again next frame.
            return;
        }
        std::memcpy(mapped.pData, scratch.data(), static_cast<size_t>(vertexCount) * sizeof(CloudScene::Vertex));
        context->Unmap(vertexBuffer.Get(), 0);
    }
    if (vertexCount == 0) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &draw.push, sizeof(CloudScene::Push));
    context->Unmap(constants.Get(), 0);

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(inputLayout.Get());
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);
    ID3D11Buffer* push = constants.Get();
    context->VSSetConstantBuffers(0, 1, &push); // Both stages read the same Push block, at b0.
    context->PSSetConstantBuffers(0, 1, &push);
    context->RSSetState(rasterizer.Get());
    context->OMSetDepthStencilState(depthState.Get(), 0);
    context->OMSetBlendState(blendState.Get(), nullptr, 0xFFFFFFFF);

    UINT stride = sizeof(CloudScene::Vertex);
    UINT offset = 0;
    ID3D11Buffer* vertices = vertexBuffer.Get();
    context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
    context->Draw(vertexCount, 0);
}

void D3D11CloudPass::ReloadTextures(const ShaderLibrary& shaders)
{
    if (!vertexBuffer)
    {
        // Nothing was created (the first load found no usable clouds.png): try the whole pass again.
        Destroy();
        Create(shaders);
        return;
    }
    if (scene.LoadCells()) mode = -1; // Rebuild the mesh from the new cells.
}

void D3D11CloudPass::Destroy()
{
    mode = -1;
    vertexCount = 0;
    scratch.clear();
    scratch.shrink_to_fit();
    blendState.Reset();
    depthState.Reset();
    rasterizer.Reset();
    constants.Reset();
    vertexBuffer.Reset();
    inputLayout.Reset();
    pixelShader.Reset();
    vertexShader.Reset();
}

} // namespace Volcano::Engine::D3D11
