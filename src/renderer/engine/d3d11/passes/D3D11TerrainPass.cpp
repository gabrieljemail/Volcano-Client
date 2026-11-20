#define NOMINMAX
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <wrl/client.h>
#include "D3D11TerrainPass.hpp"
#include "../D3D11RenderEngine.hpp"
#include "../D3D11Shader.hpp"
#include "../../../../ApplicationDeps.hpp"
import Volcano;
#include "../../../models/PackedVertex.hpp"
#include "../../../models/Mesh.hpp"
#include "../../../../Logger.hpp"

namespace Volcano::Engine::D3D11 {

namespace {

// Must match terrain.vert.hlsl's PushConstants cbuffer (b1): a mat4 plus the
// chunk fade, padded to cbuffer register alignment.
struct Push {
    glm::mat4 model;
    glm::vec4 fade;
};

} // namespace

D3D11TerrainPass::D3D11TerrainPass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
    : RenderPass(NAME), device(d3dDevice), context(d3dContext) {}

void D3D11TerrainPass::Create(const ShaderLibrary& /*shaders*/)
{
    vertexShader = CreateVertexShader(device, VERTEX_SHADER);
    pixelShader = CreatePixelShader(device, FRAGMENT_SHADER);

    // PackedVertex: one uint3 at TEXCOORD0, 12-byte stride. The semantic is
    // name "TEXCOORD" + index 0 - "TEXCOORD0" as the name doesn't match the
    // shader signature. CreateInputLayout validates against the shader
    // bytecode, so compile once here (the shader object itself comes from
    // CreateVertexShader's own compile).
    auto vsCode = CompileShader(VERTEX_SHADER);
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_UINT, 0, 0,
            D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device->CreateInputLayout(elements, 1, vsCode.data(), vsCode.size(), &inputLayout)))
    {
        throw std::runtime_error("[ERROR] Failed to create the terrain input layout.");
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(Push);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, &pushBuffer)))
    {
        throw std::runtime_error("[ERROR] Failed to create the terrain constant buffer.");
    }

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &depthState)))
    {
        throw std::runtime_error("[ERROR] Failed to create the terrain depth state.");
    }

    // Vulkan's frontFace=COUNTER_CLOCKWISE, measured in its y-down
    // framebuffer space. After the engine's proj un-flip the DX11 screen
    // positions are identical, but D3D's default front is clockwise - flip
    // the convention to keep the same faces front-facing.
    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_BACK;
    rasterizerDesc.FrontCounterClockwise = TRUE;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizerDesc, &rasterizerSolid)))
    {
        throw std::runtime_error("[ERROR] Failed to create the terrain rasterizer state.");
    }

    // F3+Z wireframe: Vulkan's twin pipeline is LINE + no culling.
    rasterizerDesc.FillMode = D3D11_FILL_WIREFRAME;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    if (FAILED(device->CreateRasterizerState(&rasterizerDesc, &rasterizerWireframe)))
    {
        throw std::runtime_error("[ERROR] Failed to create the terrain wireframe state.");
    }

    Log::Info("Terrain pass active.");
}

void D3D11TerrainPass::Record(FrameContext& frame)
{
    if (!vertexShader || !pixelShader || !inputLayout) return;

    D3D11RenderEngine* d3d = frame.engine->GetD3D11();
    if (!d3d) return;

    const bool wireframe = frame.scene->wireframe;

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(inputLayout.Get());
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);

    // b0: camera (uploaded by the engine this frame); b1: per-mesh model+fade;
    // t0/s0: the block/item texture array.
    ID3D11Buffer* camera = d3d->GetCameraConstantBuffer();
    ID3D11Buffer* push = pushBuffer.Get();
    context->VSSetConstantBuffers(0, 1, &camera);
    context->PSSetConstantBuffers(0, 1, &camera);
    context->VSSetConstantBuffers(1, 1, &push);

    ID3D11ShaderResourceView* texture = d3d->GetBlockTextureArray();
    ID3D11SamplerState* sampler = d3d->GetBlockTextureSampler();
    context->PSSetShaderResources(0, 1, &texture);
    context->PSSetSamplers(0, 1, &sampler);

    context->RSSetState((wireframe ? rasterizerWireframe : rasterizerSolid).Get());
    context->OMSetDepthStencilState(depthState.Get(), 0);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    // Locked against MeshingThread, which appends to renderList as chunks
    // stream in.
    std::lock_guard<std::mutex> lock(frame.state->renderListMutex);
    const std::vector<const Mesh*>& visible = GetVisible(cache, frame.state->renderList, frame, false);

    const auto now = std::chrono::steady_clock::now();
    Application* state = frame.state;
    Passes::ChunkCull::PhaseTimer drawTimer(state, Application::CHUNK_PHASE_DRAW);

    for (const Mesh* mesh : visible)
    {
        if (mesh->vertexBuffer == nullptr || mesh->vertexCount == 0) continue;

        ID3D11Buffer* vertexBuffer = static_cast<ID3D11Buffer*>(mesh->vertexBuffer);
        UINT stride = sizeof(PackedVertex);
        UINT vertexOffset = static_cast<UINT>(mesh->vertexOffset);
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &vertexOffset);

        Push pushData{ mesh->modelMatrix, glm::vec4(FadeAmount(*mesh, now), 0.0f, 0.0f, 0.0f) };
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(pushBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) break;
        std::memcpy(mapped.pData, &pushData, sizeof(pushData));
        context->Unmap(pushBuffer.Get(), 0);

        if (mesh->indexBuffer != nullptr && mesh->indexCount > 0)
        {
            ID3D11Buffer* indexBuffer = static_cast<ID3D11Buffer*>(mesh->indexBuffer);
            context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT,
                static_cast<UINT>(mesh->indexOffset));
            context->DrawIndexed(mesh->indexCount, 0, 0);
            if (state && state->debugHudVisible) state->trianglesDrawn[Application::TRIANGLES_TERRAIN] += mesh->indexCount / 3;
        }
        else
        {
            context->Draw(mesh->vertexCount, 0);
            if (state && state->debugHudVisible) state->trianglesDrawn[Application::TRIANGLES_TERRAIN] += mesh->vertexCount / 3;
        }
    }
}

void D3D11TerrainPass::Destroy()
{
    cache = Passes::ChunkCull::VisibleCache{};
    vertexShader.Reset();
    pixelShader.Reset();
    inputLayout.Reset();
    pushBuffer.Reset();
    depthState.Reset();
    rasterizerSolid.Reset();
    rasterizerWireframe.Reset();
}

} // namespace Volcano::Engine::D3D11
