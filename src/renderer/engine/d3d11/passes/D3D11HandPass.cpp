#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "D3D11HandPass.hpp"
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "D3D11EntityAssets.hpp"
#include "../D3D11ItemRenderer.hpp"
#include "../D3D11Shader.hpp"
#include "../../../../ApplicationDeps.hpp"
import Volcano;
#include "../../../../Logger.hpp"
#include "../../../entity/EntityScene.hpp"
#include "../../../entity/models/EntityJsonModel.hpp"

namespace Volcano::Engine::D3D11 {

bool D3D11HandPass::BuildArm(ArmMesh& mesh, const glm::vec3& boxMin, const glm::vec2& uv)
{
    std::vector<EntityJsonModel::Vertex> vertices;
    std::vector<uint32_t> indices;
    HandScene::BuildArmGeometry(boxMin, uv, vertices, indices);

    D3D11_BUFFER_DESC desc{};
    D3D11_SUBRESOURCE_DATA data{};
    desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(EntityJsonModel::Vertex));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    data.pSysMem = vertices.data();
    if (FAILED(device->CreateBuffer(&desc, &data, &mesh.vertices))) return false;
    desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = indices.data();
    if (FAILED(device->CreateBuffer(&desc, &data, &mesh.indices))) return false;
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    return true;
}

void D3D11HandPass::Create(const ShaderLibrary&)
{
    // The hand is not essential: if anything here fails, the world still renders without it.
    try
    {
        vertexShader = CreateVertexShader(device, VERTEX_SHADER);
        pixelShader = CreatePixelShader(device, FRAGMENT_SHADER);

        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(EntityJsonModel::Vertex, position)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(EntityJsonModel::Vertex, normal)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(EntityJsonModel::Vertex, uv)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        std::vector<char> bytecode = CompileShader(VERTEX_SHADER);
        if (FAILED(device->CreateInputLayout(elements, 3, bytecode.data(), bytecode.size(), &inputLayout)))
        {
            throw std::runtime_error("input layout");
        }

        D3D11_BUFFER_DESC constantDesc{};
        constantDesc.ByteWidth = sizeof(ArmConstants);
        constantDesc.Usage = D3D11_USAGE_DYNAMIC;
        constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&constantDesc, nullptr, &constants))) throw std::runtime_error("constants");

        // No culling: the box is convex and depth-tested, and the transform chain
        // mixes handedness enough that winding is not worth getting wrong.
        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rasterDesc, &rasterizer))) throw std::runtime_error("rasterizer");

        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        depthDesc.DepthEnable = TRUE;
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
        if (FAILED(device->CreateDepthStencilState(&depthDesc, &depthState))) throw std::runtime_error("depth state");

        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&blendDesc, &blendState))) throw std::runtime_error("blend state");

        if (!BuildArm(rightArm, HandScene::RIGHT_ARM_MIN, HandScene::RIGHT_ARM_UV)
            || !BuildArm(leftArm, HandScene::LEFT_ARM_MIN, HandScene::LEFT_ARM_UV))
        {
            throw std::runtime_error("arm meshes");
        }
        ready = true;
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[WARN] First-person hand unavailable on Direct3D 11: ") + e.what());
        Destroy();
    }
}

void D3D11HandPass::DrawArm(const ArmMesh& mesh, const glm::mat4& mvp, float brightness,
    ID3D11ShaderResourceView* skin, ID3D11SamplerState* sampler)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    ArmConstants data{};
    std::memcpy(data.mvp, &mvp, sizeof(data.mvp));
    data.shade[0] = brightness;
    std::memcpy(mapped.pData, &data, sizeof(data));
    context->Unmap(constants.Get(), 0);

    UINT stride = sizeof(EntityJsonModel::Vertex);
    UINT offset = 0;
    ID3D11Buffer* vertices = mesh.vertices.Get();
    context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
    context->IASetIndexBuffer(mesh.indices.Get(), DXGI_FORMAT_R32_UINT, 0);

    context->PSSetShaderResources(0, 1, &skin);
    context->PSSetSamplers(0, 1, &sampler);
    context->DrawIndexed(mesh.indexCount, 0, 0);
}

void D3D11HandPass::Record(FrameContext& frame)
{
    Application* state = frame.state;
    D3D11ItemRenderer* items = state != nullptr ? state->d3dItemRenderer : nullptr;
    if (!ready || items == nullptr) return;
    if (!scene.Update(frame, false, hands)) return;

    float width = static_cast<float>(frame.extent.width);
    float height = static_cast<float>(frame.extent.height);
    D3D11_VIEWPORT handViewport{ 0.0f, 0.0f, width, height, 0.0f, HandScene::MAX_DEPTH };
    context->RSSetViewports(1, &handViewport);

    if (!hands.arms.empty())
    {
        // The local player's own skin once it has downloaded, the default one until then.
        D3D11EntityAssets* assets = items->GetEntityAssets();
        ID3D11ShaderResourceView* skinView = nullptr;
        ID3D11SamplerState* sampler = nullptr;
        if (assets != nullptr)
        {
            const EntityScene::MobModel* unusedModel = nullptr;
            const EntityScene::SkinBase* local = nullptr;
            assets->ResolvePlayerLook(state, EntityScene::LOCAL_SKIN_KEY, unusedModel, local);
            const D3D11EntityAssets::Skin* skin = local != nullptr ? &D3D11EntityAssets::AsD3D(*local) : assets->DefaultSkin();
            if (skin != nullptr) skinView = skin->view.Get();
            sampler = assets->GetSampler();
        }

        if (skinView != nullptr && sampler != nullptr)
        {
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->IASetInputLayout(inputLayout.Get());
            context->VSSetShader(vertexShader.Get(), nullptr, 0);
            context->PSSetShader(pixelShader.Get(), nullptr, 0);
            ID3D11Buffer* buffer = constants.Get();
            context->VSSetConstantBuffers(0, 1, &buffer);
            context->RSSetState(rasterizer.Get());
            context->OMSetDepthStencilState(depthState.Get(), 0);
            context->OMSetBlendState(blendState.Get(), nullptr, 0xFFFFFFFF);
            for (const HandScene::Arm& arm : hands.arms)
            {
                DrawArm(arm.right ? rightArm : leftArm, arm.mvp, hands.handLight, skinView, sampler);
            }
        }
    }

    for (const HandScene::Item& item : hands.items)
    {
        items->DrawItem(item.name, item.context, item.placement, hands.proj, item.pivotRotation, item.darkness,
            item.glint);
    }

    D3D11_VIEWPORT fullViewport{ 0.0f, 0.0f, width, height, 0.0f, 1.0f };
    context->RSSetViewports(1, &fullViewport);
}

void D3D11HandPass::Destroy()
{
    ready = false;
    rightArm = ArmMesh{};
    leftArm = ArmMesh{};
    blendState.Reset();
    depthState.Reset();
    rasterizer.Reset();
    constants.Reset();
    inputLayout.Reset();
    pixelShader.Reset();
    vertexShader.Reset();
}

} // namespace Volcano::Engine::D3D11
