#define NOMINMAX
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include "D3D11NonCubicPass.hpp"
#include "../D3D11RenderEngine.hpp"
#include "../D3D11Shader.hpp"
#include "../../../../ApplicationDeps.hpp"
import Volcano;
#include "../../../models/Mesh.hpp"
#include "../../../misc/MiscVertex.hpp"
#include "../../../../Logger.hpp"

namespace Volcano::Engine::D3D11 {

namespace {

// Must match misc.vert.hlsl's PushConstants cbuffer (b1): a mat4 plus the
// chunk fade, padded to cbuffer register alignment.
struct Push {
    glm::mat4 model;
    glm::vec4 fade;
};

} // namespace

D3D11NonCubicPass::D3D11NonCubicPass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
    : RenderPass(NAME), device(d3dDevice), context(d3dContext) {}

void D3D11NonCubicPass::Create(const ShaderLibrary& /*shaders*/)
{
    vertexShader = CreateVertexShader(device, VERTEX_SHADER);
    pixelShader = CreatePixelShader(device, FRAGMENT_SHADER);

    // MiscVertex: float3 position, float2 uv, uint packed (24 bytes). The
    // semantics match misc.vert.hlsl's TEXCOORD0..2 inputs.
    auto vsCode = CompileShader(VERTEX_SHADER);
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MiscVertex, position),
            D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(MiscVertex, uv),
            D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, offsetof(MiscVertex, packed),
            D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device->CreateInputLayout(elements, 3, vsCode.data(), vsCode.size(), &inputLayout)))
    {
        throw std::runtime_error("[ERROR] Failed to create the non-cubic input layout.");
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(Push);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, &pushBuffer)))
    {
        throw std::runtime_error("[ERROR] Failed to create the non-cubic constant buffer.");
    }

    // Cutout writes depth (stairs/slabs are opaque solids that merely don't
    // fill their voxel); the translucent phase tests but doesn't write.
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &depthWrite)))
    {
        throw std::runtime_error("[ERROR] Failed to create the non-cubic depth state.");
    }
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &depthNoWrite)))
    {
        throw std::runtime_error("[ERROR] Failed to create the translucent depth state.");
    }

    // No culling: thin geometry (cross billboards, carpets) must show from
    // both sides, same as the Vulkan pipelines.
    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizerDesc, &rasterizer)))
    {
        throw std::runtime_error("[ERROR] Failed to create the non-cubic rasterizer state.");
    }

    D3D11_BLEND_DESC blendDesc{};
    auto& target = blendDesc.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blendDesc, &blend)))
    {
        throw std::runtime_error("[ERROR] Failed to create the non-cubic blend state.");
    }

    Log::Info("Non-cubic pass active.");
}

void D3D11NonCubicPass::DrawList(const std::vector<const Mesh*>& meshes, Application* state,
    uint32_t triangleCategory)
{
    const auto now = std::chrono::steady_clock::now();
    for (const Mesh* mesh : meshes)
    {
        if (mesh->vertexBuffer == nullptr || mesh->vertexCount == 0) continue;

        ID3D11Buffer* vertexBuffer = static_cast<ID3D11Buffer*>(mesh->vertexBuffer);
        UINT stride = sizeof(MiscVertex);
        UINT vertexOffset = static_cast<UINT>(mesh->vertexOffset);
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &vertexOffset);

        Push pushData{ mesh->modelMatrix, glm::vec4(FadeAmount(*mesh, now), 0.0f, 0.0f, 0.0f) };
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(pushBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) break;
        std::memcpy(mapped.pData, &pushData, sizeof(pushData));
        context->Unmap(pushBuffer.Get(), 0);

        const bool count = state && state->debugHudVisible;
        if (mesh->indexBuffer != nullptr && mesh->indexCount > 0)
        {
            ID3D11Buffer* indexBuffer = static_cast<ID3D11Buffer*>(mesh->indexBuffer);
            context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT,
                static_cast<UINT>(mesh->indexOffset));
            context->DrawIndexed(mesh->indexCount, 0, 0);
            if (count) state->trianglesDrawn[triangleCategory] += mesh->indexCount / 3;
        }
        else
        {
            context->Draw(mesh->vertexCount, 0);
            if (count) state->trianglesDrawn[triangleCategory] += mesh->vertexCount / 3;
        }
    }
}

void D3D11NonCubicPass::Record(FrameContext& frame)
{
    if (!vertexShader || !pixelShader || !inputLayout) return;

    D3D11RenderEngine* d3d = frame.engine->GetD3D11();
    if (!d3d) return;

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(inputLayout.Get());
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);

    ID3D11Buffer* camera = d3d->GetCameraConstantBuffer();
    ID3D11Buffer* push = pushBuffer.Get();
    context->VSSetConstantBuffers(0, 1, &camera);
    context->PSSetConstantBuffers(0, 1, &camera);
    context->VSSetConstantBuffers(1, 1, &push);

    ID3D11ShaderResourceView* texture = d3d->GetBlockTextureArray();
    ID3D11SamplerState* sampler = d3d->GetBlockTextureSampler();
    context->PSSetShaderResources(0, 1, &texture);
    context->PSSetSamplers(0, 1, &sampler);

    context->RSSetState(rasterizer.Get());
    context->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFF);

    Application* state = frame.state;

    // Cutout phase. The list mutex is released before the translucent phase
    // so MeshingThread's upserts into this list aren't blocked for the whole
    // translucent draw (same as the Vulkan pass).
    {
        std::lock_guard<std::mutex> lock(state->nonCubicRenderListMutex);
        context->OMSetDepthStencilState(depthWrite.Get(), 0);
        const std::vector<const Mesh*>& meshes =
            GetVisible(cutoutCache, state->nonCubicRenderList, frame, true);
        PhaseTimer drawTimer(state, Application::CHUNK_PHASE_DRAW);
        DrawList(meshes, state, Application::TRIANGLES_CUTOUT);
    }

    // Translucent phase (water): depth test on, writes off, drawn right after
    // the cutout geometry so it blends with it.
    {
        std::lock_guard<std::mutex> lock(state->translucentRenderListMutex);
        context->OMSetDepthStencilState(depthNoWrite.Get(), 0);
        const std::vector<const Mesh*>& meshes =
            GetVisible(translucentCache, state->translucentRenderList, frame, true);
        PhaseTimer drawTimer(state, Application::CHUNK_PHASE_DRAW);
        DrawList(meshes, state, Application::TRIANGLES_TRANSLUCENT);
    }
}

void D3D11NonCubicPass::Destroy()
{
    cutoutCache = Passes::ChunkCull::VisibleCache{};
    translucentCache = Passes::ChunkCull::VisibleCache{};
    vertexShader.Reset();
    pixelShader.Reset();
    inputLayout.Reset();
    pushBuffer.Reset();
    depthWrite.Reset();
    depthNoWrite.Reset();
    rasterizer.Reset();
    blend.Reset();
}

} // namespace Volcano::Engine::D3D11
