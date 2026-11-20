#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "D3D11SelectionPass.hpp"
#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <stb_image.h>
#include <glm/glm.hpp>
#include "../D3D11RenderEngine.hpp"
#include "../D3D11Shader.hpp"
#include "../../../../Logger.hpp"
#include "../../../../resources/ImageLoad.hpp"

namespace Volcano::Engine::D3D11 {

namespace {

struct CrackVertex {
    glm::vec3 position;
    glm::vec2 uv;
};

constexpr uint32_t FILL_INDEX_COUNT = 36; // 6 faces * 2 triangles * 3.
constexpr uint32_t LINE_INDEX_COUNT = 24; // 12 edges * 2.
constexpr uint32_t CRACK_INDEX_COUNT = 36;

void Check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string("[ERROR] D3D11SelectionPass: ") + what + " failed.");
}

Microsoft::WRL::ComPtr<ID3D11Buffer> ImmutableBuffer(ID3D11Device* device, const void* data, size_t bytes, UINT bind)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(bytes);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{ data, 0, 0 };
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    Check(device->CreateBuffer(&desc, &init, &buffer), "CreateBuffer");
    return buffer;
}

Microsoft::WRL::ComPtr<ID3D11Buffer> ConstantBuffer(ID3D11Device* device, size_t bytes)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>((bytes + 15) & ~size_t{15});
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    Check(device->CreateBuffer(&desc, nullptr, &buffer), "CreateBuffer(constants)");
    return buffer;
}

} // namespace

bool D3D11SelectionPass::BuildBuffers()
{
    // The unit cube (8 corners, index = bit0 x, bit1 y, bit2 z) with the 6-face
    // fill and the 12-edge outline; every draw repositions it with constants.
    const std::array<glm::vec3, 8> corners = {{
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1},
    }};
    const std::array<uint16_t, FILL_INDEX_COUNT> fill = {{
        0,2,6, 0,6,4, 1,5,7, 1,7,3, 0,1,5, 0,5,4, 2,6,7, 2,7,3, 0,1,3, 0,3,2, 4,5,7, 4,7,6,
    }};
    const std::array<uint16_t, LINE_INDEX_COUNT> lines = {{
        0,1, 0,2, 0,4, 1,3, 1,5, 2,3, 2,6, 3,7, 4,5, 4,6, 5,7, 6,7,
    }};
    cubeVertices = ImmutableBuffer(device, corners.data(), sizeof(corners), D3D11_BIND_VERTEX_BUFFER);
    fillIndices = ImmutableBuffer(device, fill.data(), sizeof(fill), D3D11_BIND_INDEX_BUFFER);
    lineIndices = ImmutableBuffer(device, lines.data(), sizeof(lines), D3D11_BIND_INDEX_BUFFER);

    // The crack cube: 24 vertices so each face gets its own UVs.
    std::array<CrackVertex, 24> vertices{};
    std::array<uint16_t, CRACK_INDEX_COUNT> indices{};
    for (int face = 0; face < 6; face++)
    {
        int normalAxis = face / 2, uAxis = (normalAxis + 1) % 3, vAxis = (normalAxis + 2) % 3;
        const float quad[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int c = 0; c < 4; c++)
        {
            CrackVertex& vertex = vertices[static_cast<size_t>(face * 4 + c)];
            vertex.position[normalAxis] = static_cast<float>(face % 2);
            vertex.position[uAxis] = quad[c][0];
            vertex.position[vAxis] = quad[c][1];
            vertex.uv = glm::vec2(quad[c][0], 1.0f - quad[c][1]);
        }
        const uint16_t base = static_cast<uint16_t>(face * 4);
        const uint16_t order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < 6; i++) indices[static_cast<size_t>(face * 6 + i)] = static_cast<uint16_t>(base + order[i]);
    }
    crackVertices = ImmutableBuffer(device, vertices.data(), sizeof(vertices), D3D11_BIND_VERTEX_BUFFER);
    crackIndices = ImmutableBuffer(device, indices.data(), sizeof(indices), D3D11_BIND_INDEX_BUFFER);
    return true;
}

void D3D11SelectionPass::Create(const ShaderLibrary&)
{
    // The outline is a nicety: if anything here fails the world still renders without it.
    try
    {
        selectionVertex = CreateVertexShader(device, "selection.vert");
        selectionPixel = CreatePixelShader(device, "selection.frag");
        const D3D11_INPUT_ELEMENT_DESC selectionElements[] = {
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        std::vector<char> selectionBytecode = CompileShader("selection.vert");
        Check(device->CreateInputLayout(selectionElements, 1, selectionBytecode.data(), selectionBytecode.size(),
            &selectionLayout), "CreateInputLayout(selection)");

        BuildBuffers();
        selectionConstants = ConstantBuffer(device, sizeof(BoxConstants));
        crackConstants = ConstantBuffer(device, 2 * 4 * sizeof(float));

        // Both the fill and the outline are translucent: standard alpha blend.
        D3D11_BLEND_DESC alpha{};
        alpha.RenderTarget[0].BlendEnable = TRUE;
        alpha.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        alpha.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        alpha.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        alpha.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        alpha.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        alpha.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        alpha.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        Check(device->CreateBlendState(&alpha, &alphaBlend), "CreateBlendState(alpha)");

        // No culling. The depth bias pulls the overlay toward the camera, in depth-buffer units, so it
        // stays in front of the coplanar block face at any distance (a fixed world offset shrinks to
        // nothing in depth terms at range). Same -4/-4 as the Vulkan pipelines.
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthBias = -4;
        raster.SlopeScaledDepthBias = -4.0f;
        raster.DepthClipEnable = TRUE;
        Check(device->CreateRasterizerState(&raster, &rasterizer), "CreateRasterizerState");

        // Tested against what the other passes drew, never writes depth.
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_LESS;
        Check(device->CreateDepthStencilState(&depth, &depthState), "CreateDepthStencilState");
        ready = true;
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[WARN] Block selection unavailable on Direct3D 11: ") + e.what());
        Destroy();
        return;
    }

    // The crack overlay is optional on top of that.
    try
    {
        crackVertex = CreateVertexShader(device, "crack.vert");
        crackPixel = CreatePixelShader(device, "crack.frag");
        const D3D11_INPUT_ELEMENT_DESC crackElements[] = {
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(CrackVertex, position)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(CrackVertex, uv)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        std::vector<char> crackBytecode = CompileShader("crack.vert");
        Check(device->CreateInputLayout(crackElements, 2, crackBytecode.data(), crackBytecode.size(), &crackLayout),
            "CreateInputLayout(crack)");

        // Multiplicative overlay (dst * src * 2): mid-gray leaves the block alone, crack pixels darken it.
        D3D11_BLEND_DESC multiply{};
        multiply.RenderTarget[0].BlendEnable = TRUE;
        multiply.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
        multiply.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
        multiply.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        multiply.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        multiply.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        multiply.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        multiply.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        Check(device->CreateBlendState(&multiply, &multiplyBlend), "CreateBlendState(multiply)");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&sampler, &crackSampler), "CreateSamplerState(crack)");
        crackReady = true;
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[WARN] Block-break cracks unavailable on Direct3D 11: ") + e.what());
        crackVertex.Reset();
        crackPixel.Reset();
        crackLayout.Reset();
        multiplyBlend.Reset();
        crackSampler.Reset();
    }
}

ID3D11ShaderResourceView* D3D11SelectionPass::CrackView(int stage)
{
    if (stage < 0 || stage >= CRACK_STAGES) return nullptr;
    size_t index = static_cast<size_t>(stage);
    if (crackTried[index]) return crackViews[index].Get();
    crackTried[index] = true; // A texture the pack lacks isn't retried every frame.

    int width = 0, height = 0;
    uint8_t* pixels = LoadImageRGBA("assets/minecraft/textures/block/destroy_stage_" + std::to_string(stage) + ".png",
        width, height);
    if (pixels == nullptr) return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{ pixels, static_cast<UINT>(width) * 4, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)))
    {
        device->CreateShaderResourceView(texture.Get(), nullptr, &crackViews[index]);
    }
    stbi_image_free(pixels);
    return crackViews[index].Get();
}

void D3D11SelectionPass::SetBox(ID3D11Buffer* buffer, const SelectionScene::Box& box, const float* color)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    BoxConstants constants{};
    constants.worldMin[0] = box.min.x; constants.worldMin[1] = box.min.y; constants.worldMin[2] = box.min.z;
    constants.worldMax[0] = box.max.x; constants.worldMax[1] = box.max.y; constants.worldMax[2] = box.max.z;
    if (color != nullptr) std::memcpy(constants.color, color, sizeof(constants.color));
    // The crack block has no color member; its buffer is just the two boxes.
    std::memcpy(mapped.pData, &constants, color != nullptr ? sizeof(BoxConstants) : 2 * 4 * sizeof(float));
    context->Unmap(buffer, 0);
}

void D3D11SelectionPass::Record(FrameContext& frame)
{
    if (!ready || frame.state == nullptr || frame.state->interaction == nullptr || frame.state->world == nullptr) return;
    D3D11RenderEngine* d3d = frame.engine->GetD3D11();
    if (d3d == nullptr) return;

    SelectionScene::Collect(frame.state, scene);
    if (scene.cracks.empty() && scene.boxes.empty()) return;

    ID3D11Buffer* camera = d3d->GetCameraConstantBuffer();
    context->VSSetConstantBuffers(0, 1, &camera);
    context->RSSetState(rasterizer.Get());
    context->OMSetDepthStencilState(depthState.Get(), 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Cracks first, so the outline draws over them.
    if (crackReady)
    {
        context->IASetInputLayout(crackLayout.Get());
        context->VSSetShader(crackVertex.Get(), nullptr, 0);
        context->PSSetShader(crackPixel.Get(), nullptr, 0);
        context->OMSetBlendState(multiplyBlend.Get(), nullptr, 0xFFFFFFFF);
        ID3D11Buffer* push = crackConstants.Get();
        context->VSSetConstantBuffers(1, 1, &push);
        ID3D11SamplerState* sampler = crackSampler.Get();
        context->PSSetSamplers(0, 1, &sampler);
        UINT stride = sizeof(CrackVertex);
        UINT offset = 0;
        ID3D11Buffer* vertices = crackVertices.Get();
        context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
        context->IASetIndexBuffer(crackIndices.Get(), DXGI_FORMAT_R16_UINT, 0);
        for (const SelectionScene::Crack& crack : scene.cracks)
        {
            ID3D11ShaderResourceView* view = CrackView(crack.stage);
            if (view == nullptr) continue;
            context->PSSetShaderResources(0, 1, &view);
            for (const SelectionScene::Box& box : crack.boxes)
            {
                SetBox(crackConstants.Get(), box, nullptr);
                context->DrawIndexed(CRACK_INDEX_COUNT, 0, 0);
            }
        }
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
    }

    if (scene.boxes.empty()) return;

    context->IASetInputLayout(selectionLayout.Get());
    context->VSSetShader(selectionVertex.Get(), nullptr, 0);
    context->PSSetShader(selectionPixel.Get(), nullptr, 0);
    context->OMSetBlendState(alphaBlend.Get(), nullptr, 0xFFFFFFFF);
    ID3D11Buffer* push = selectionConstants.Get();
    context->VSSetConstantBuffers(1, 1, &push);
    UINT stride = sizeof(glm::vec3);
    UINT offset = 0;
    ID3D11Buffer* vertices = cubeVertices.Get();
    context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);

    for (const SelectionScene::Box& box : scene.boxes)
    {
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetIndexBuffer(fillIndices.Get(), DXGI_FORMAT_R16_UINT, 0);
        SetBox(selectionConstants.Get(), box, &scene.fillColor.x);
        context->DrawIndexed(FILL_INDEX_COUNT, 0, 0);

        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        context->IASetIndexBuffer(lineIndices.Get(), DXGI_FORMAT_R16_UINT, 0);
        SetBox(selectionConstants.Get(), box, &scene.outlineColor.x);
        context->DrawIndexed(LINE_INDEX_COUNT, 0, 0);
    }
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void D3D11SelectionPass::ClearCrackTextures()
{
    for (auto& view : crackViews) view.Reset();
    crackTried.fill(false);
}

void D3D11SelectionPass::ReloadTextures(const ShaderLibrary&)
{
    ClearCrackTextures();
}

void D3D11SelectionPass::Destroy()
{
    ready = false;
    crackReady = false;
    selectionVertex.Reset();
    selectionPixel.Reset();
    selectionLayout.Reset();
    cubeVertices.Reset();
    fillIndices.Reset();
    lineIndices.Reset();
    selectionConstants.Reset();
    alphaBlend.Reset();
    crackVertex.Reset();
    crackPixel.Reset();
    crackLayout.Reset();
    crackVertices.Reset();
    crackIndices.Reset();
    crackConstants.Reset();
    multiplyBlend.Reset();
    crackSampler.Reset();
    ClearCrackTextures();
    rasterizer.Reset();
    depthState.Reset();
}

} // namespace Volcano::Engine::D3D11
