#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "D3D11ParticlePass.hpp"
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <glm/glm.hpp>
#include "../D3D11Shader.hpp"
#include "../../../../ApplicationDeps.hpp"
import Volcano;
#include "../../../../Logger.hpp"

namespace Volcano::Engine::D3D11 {

namespace {

void Check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string("[ERROR] D3D11ParticlePass: ") + what + " failed.");
}

} // namespace

void D3D11ParticlePass::Create(const ShaderLibrary&)
{
    // Particles are decoration: if anything here fails the world still renders without them.
    try
    {
        vertexShader = CreateVertexShader(device, VERTEX_SHADER);
        pixelShader = CreatePixelShader(device, FRAGMENT_SHADER);

        // One instance per particle (the quad's six corners come from SV_VertexID).
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, static_cast<UINT>(offsetof(ParticleInstance, positionSize)), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, static_cast<UINT>(offsetof(ParticleInstance, color)), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, static_cast<UINT>(offsetof(ParticleInstance, uv)), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(ParticleInstance, layer)), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        };
        std::vector<char> bytecode = CompileShader(VERTEX_SHADER);
        Check(device->CreateInputLayout(elements, 4, bytecode.data(), bytecode.size(), &inputLayout), "CreateInputLayout");

        D3D11_BUFFER_DESC instanceDesc{};
        instanceDesc.ByteWidth = static_cast<UINT>(sizeof(ParticleInstance) * ParticleSystem::MAX_PARTICLES);
        instanceDesc.Usage = D3D11_USAGE_DYNAMIC;
        instanceDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        instanceDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&instanceDesc, nullptr, &instanceBuffer), "CreateBuffer(instances)");

        D3D11_BUFFER_DESC constantDesc{};
        constantDesc.ByteWidth = (sizeof(Push) + 15u) & ~15u;
        constantDesc.Usage = D3D11_USAGE_DYNAMIC;
        constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&constantDesc, nullptr, &constants), "CreateBuffer(constants)");

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&samplerDesc, &sampler), "CreateSamplerState");

        // A billboard faces the camera whichever way its triangles wind.
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        Check(device->CreateRasterizerState(&raster, &rasterizer), "CreateRasterizerState");

        // Behind terrain and entities, never writing depth.
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

        scratch.resize(ParticleSystem::MAX_PARTICLES);
        Log::Info("Particle pass active (Direct3D 11).");
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[WARN] Particles unavailable on Direct3D 11: ") + e.what());
        Destroy();
    }
}

bool D3D11ParticlePass::UploadAtlas(const ParticleSprites& sprites)
{
    const uint32_t layerCount = sprites.LayerCount();
    const uint32_t size = ParticleSprites::LAYER_SIZE;
    if (layerCount == 0 || layerCount > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) return false;
    if (sprites.Pixels().size() < static_cast<size_t>(layerCount) * size * size * 4) return false;

    // One subresource per layer, each LAYER_SIZE x LAYER_SIZE, no mips.
    std::vector<D3D11_SUBRESOURCE_DATA> layers(layerCount);
    const size_t layerBytes = static_cast<size_t>(size) * size * 4;
    for (uint32_t layer = 0; layer < layerCount; layer++)
    {
        layers[layer].pSysMem = sprites.Pixels().data() + layer * layerBytes;
        layers[layer].SysMemPitch = size * 4;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size;
    desc.Height = size;
    desc.MipLevels = 1;
    desc.ArraySize = layerCount;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // The same gamma-space convention as the block textures.
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, layers.data(), &texture)))
    {
        Log::Error("[ERROR] Particles: couldn't create the atlas texture.");
        return false;
    }
    if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &atlasView)))
    {
        Log::Error("[ERROR] Particles: couldn't create the atlas view.");
        return false;
    }
    return true;
}

void D3D11ParticlePass::Record(FrameContext& frame)
{
    if (!instanceBuffer || frame.state == nullptr) return;
    ParticleSystem* system = frame.state->particles;
    if (system == nullptr) return;
    const Engine::SceneView& scene = *frame.scene;
    if (scene.menuBackground) return;

    if (!atlasView)
    {
        if (atlasFailed) return;
        if (!UploadAtlas(system->Sprites()))
        {
            atlasFailed = true;
            return;
        }
    }

    system->Update(frame.state, scene.deltaTime);

    size_t count = system->BuildInstances(scene.cameraPosition, scene.dayBrightness, scratch.data(),
        ParticleSystem::MAX_PARTICLES);
    if (count == 0) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, scratch.data(), count * sizeof(ParticleInstance));
    context->Unmap(instanceBuffer.Get(), 0);

    // Positions are camera-relative, so the matrix carries only the view's rotation.
    Push push{};
    glm::mat4 viewProj = scene.proj * glm::mat4(glm::mat3(scene.view));
    std::memcpy(push.viewProj, &viewProj, sizeof(push.viewProj));
    push.right[0] = scene.view[0][0]; push.right[1] = scene.view[1][0]; push.right[2] = scene.view[2][0];
    push.up[0] = scene.view[0][1]; push.up[1] = scene.view[1][1]; push.up[2] = scene.view[2][1];
    if (FAILED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &push, sizeof(push));
    context->Unmap(constants.Get(), 0);

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(inputLayout.Get());
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);
    ID3D11Buffer* pushBuffer = constants.Get();
    context->VSSetConstantBuffers(0, 1, &pushBuffer);
    ID3D11ShaderResourceView* view = atlasView.Get();
    ID3D11SamplerState* samplerState = sampler.Get();
    context->PSSetShaderResources(0, 1, &view);
    context->PSSetSamplers(0, 1, &samplerState);
    context->RSSetState(rasterizer.Get());
    context->OMSetDepthStencilState(depthState.Get(), 0);
    context->OMSetBlendState(blendState.Get(), nullptr, 0xFFFFFFFF);

    UINT stride = sizeof(ParticleInstance);
    UINT offset = 0;
    ID3D11Buffer* instances = instanceBuffer.Get();
    context->IASetVertexBuffers(0, 1, &instances, &stride, &offset);
    context->DrawInstanced(6, static_cast<UINT>(count), 0, 0);

    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
}

void D3D11ParticlePass::ReloadTextures(const ShaderLibrary&)
{
    atlasView.Reset();
    atlasFailed = false;
}

void D3D11ParticlePass::Destroy()
{
    atlasView.Reset();
    atlasFailed = false;
    scratch.clear();
    scratch.shrink_to_fit();
    blendState.Reset();
    depthState.Reset();
    rasterizer.Reset();
    sampler.Reset();
    constants.Reset();
    instanceBuffer.Reset();
    inputLayout.Reset();
    pixelShader.Reset();
    vertexShader.Reset();
}

} // namespace Volcano::Engine::D3D11
