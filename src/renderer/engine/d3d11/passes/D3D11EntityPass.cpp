#define NOMINMAX
#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>
#include "D3D11EntityPass.hpp"
#include "D3D11EntityAssets.hpp"
#include "../D3D11RenderEngine.hpp"
#include "../D3D11ItemRenderer.hpp"
#include "../D3D11Shader.hpp"
#include "../../../../ApplicationDeps.hpp"
import Volcano;
#include "../../../../Logger.hpp"
#include "../../../entity/EntityScene.hpp"
#include "../../../entity/models/EntityJsonModel.hpp"
#include "../../../entity/models/Entity.hpp"
#include "../../../entity/models/EntityBoxMesh.hpp"
#include "../../../entity/models/EntityRegistry.hpp"
#include "../../../entity/models/EntitySeat.hpp"
#include "../../../entity/models/QuadrupedModel.hpp"
#include "../../../entity/models/GolemModel.hpp"
#include "../../../entity/models/PigModel.hpp"
#include "../../../entity/models/CowModel.hpp"

namespace Volcano::Engine::D3D11 {

namespace {

using EntityScene::GPUEntityInstance;
static_assert(sizeof(GPUEntityInstance) == 64, "entity.vert strides instances by 64 bytes");

// Stage 1 has no textured models: report none, so EntityScene routes every
// entity down the flat-shape path (and skips the local player's body).
class NoModelAssets final : public EntityScene::Assets {
public:
    const EntityScene::MobModel* FindMob(const std::string&) const override { return nullptr; }
    void ResolvePlayerLook(Application*, const std::string&, const EntityScene::MobModel*&,
        const EntityScene::SkinBase*&) override {}
    const EntityScene::SkinBase* ArmorSkin(const std::string&) override { return nullptr; }
    const EntityScene::SkinBase* ArmorGlintSkin() override { return nullptr; }
};

} // namespace

D3D11EntityPass::D3D11EntityPass(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
    : RenderPass(NAME), device(d3dDevice), context(d3dContext) {}

void D3D11EntityPass::CreateShape(Shape shape, const void* vertices, size_t vertexBytes,
    const std::vector<uint32_t>& indices)
{
    ShapeResources& resources = shapes[shape];
    resources.indexCount = static_cast<uint32_t>(indices.size());

    D3D11_BUFFER_DESC desc{};
    D3D11_SUBRESOURCE_DATA data{};

    desc.ByteWidth = static_cast<UINT>(vertexBytes);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    data.pSysMem = vertices;
    if (FAILED(device->CreateBuffer(&desc, &data, &resources.vertexBuffer)))
    {
        throw std::runtime_error("[ERROR] Failed to create an entity vertex buffer.");
    }

    desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = indices.data();
    if (FAILED(device->CreateBuffer(&desc, &data, &resources.indexBuffer)))
    {
        throw std::runtime_error("[ERROR] Failed to create an entity index buffer.");
    }

    // The instances are rewritten every frame with UpdateSubresource (DEFAULT
    // usage, so a raw view is always allowed).
    D3D11_BUFFER_DESC instanceDesc{};
    instanceDesc.ByteWidth = MAX_VISIBLE_ENTITIES * sizeof(GPUEntityInstance);
    instanceDesc.Usage = D3D11_USAGE_DEFAULT;
    instanceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    instanceDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(device->CreateBuffer(&instanceDesc, nullptr, &resources.instanceBuffer)))
    {
        throw std::runtime_error("[ERROR] Failed to create an entity instance buffer.");
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    viewDesc.BufferEx.FirstElement = 0;
    viewDesc.BufferEx.NumElements = instanceDesc.ByteWidth / 4;
    viewDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(device->CreateShaderResourceView(resources.instanceBuffer.Get(), &viewDesc, &resources.instanceView)))
    {
        throw std::runtime_error("[ERROR] Failed to create an entity instance view.");
    }
}

void D3D11EntityPass::Create(const ShaderLibrary& /*shaders*/)
{
    vertexShader = CreateVertexShader(device, VERTEX_SHADER);
    pixelShader = CreatePixelShader(device, FRAGMENT_SHADER);

    // EntityMeshVertex: float3 position, float3 normal (24 bytes); the
    // semantics match entity.vert.hlsl's TEXCOORD0/1 inputs.
    auto vsCode = CompileShader(VERTEX_SHADER);
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(EntityMeshVertex, position),
            D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(EntityMeshVertex, normal),
            D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device->CreateInputLayout(elements, 2, vsCode.data(), vsCode.size(), &inputLayout)))
    {
        throw std::runtime_error("[ERROR] Failed to create the entity input layout.");
    }

    auto build = [&](Shape shape, auto&& builder)
    {
        std::vector<EntityMeshVertex> vertices;
        std::vector<uint32_t> indices;
        builder(vertices, indices);
        CreateShape(shape, vertices.data(), vertices.size() * sizeof(EntityMeshVertex), indices);
    };
    build(SHAPE_CUBE, [](auto& v, auto& i) { AppendBox(v, i, glm::vec3(0.0f), glm::vec3(1.0f)); });
    build(SHAPE_QUADRUPED, [](auto& v, auto& i) { QuadrupedModel::BuildMesh(v, i); });
    build(SHAPE_GOLEM, [](auto& v, auto& i) { GolemModel::BuildMesh(v, i); });
    build(SHAPE_PIG, [](auto& v, auto& i) { PigModel::BuildMesh(v, i); });
    build(SHAPE_COW, [](auto& v, auto& i) { CowModel::BuildMesh(v, i); });

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &depthState)))
    {
        throw std::runtime_error("[ERROR] Failed to create the entity depth state.");
    }

    // Same winding convention as the terrain pass (Vulkan CCW, see there).
    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_BACK;
    rasterizerDesc.FrontCounterClockwise = TRUE;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizerDesc, &rasterizer)))
    {
        throw std::runtime_error("[ERROR] Failed to create the entity rasterizer state.");
    }

    // The textured path is optional: if its assets or shaders fail, entities
    // keep drawing as flat shapes instead of taking the renderer down.
    try
    {
        CreateTexturedPath();
        Log::Info("Entity pass active (flat shapes and textured models).");
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[WARN] Textured entities unavailable, drawing flat shapes: ") + e.what());
        assets.reset();
        texturedVertexShader.Reset();
        texturedPixelShader.Reset();
        texturedLayout.Reset();
        texturedInstanceBuffer.Reset();
        Log::Info("Entity pass active (flat-shaded shapes only).");
    }
}

void D3D11EntityPass::CreateTexturedPath()
{
    auto loaded = std::make_unique<D3D11EntityAssets>(device);
    loaded->Load();

    texturedVertexShader = CreateVertexShader(device, TEXTURED_VERTEX_SHADER);
    texturedPixelShader = CreatePixelShader(device, TEXTURED_FRAGMENT_SHADER);

    // Slot 0: EntityJsonModel::Vertex (position, normal, uv). Slot 1: one
    // TexturedInstance per drawn part, advanced per instance. The semantics
    // match entity_textured.vert.hlsl's TEXCOORD0..8.
    auto vsCode = CompileShader(TEXTURED_VERTEX_SHADER);
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(EntityJsonModel::Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(EntityJsonModel::Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(EntityJsonModel::Vertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(EntityScene::TexturedInstance, positionYaw), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(EntityScene::TexturedInstance, hurtColor), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(EntityScene::TexturedInstance, partPivot), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(EntityScene::TexturedInstance, partOffset), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 7, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(EntityScene::TexturedInstance, partRotation), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 8, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(EntityScene::TexturedInstance, bodyPose), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    if (FAILED(device->CreateInputLayout(elements, static_cast<UINT>(std::size(elements)), vsCode.data(),
            vsCode.size(), &texturedLayout)))
    {
        throw std::runtime_error("[ERROR] Failed to create the textured entity input layout.");
    }

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = MAX_TEXTURED_INSTANCES * sizeof(EntityScene::TexturedInstance);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &texturedInstanceBuffer)))
    {
        throw std::runtime_error("[ERROR] Failed to create the textured entity instance buffer.");
    }

    // The glint is optional on top of the textured path: without it armor
    // draws normally and just doesn't shimmer.
    try
    {
        glintPixelShader = CreatePixelShader(device, GLINT_FRAGMENT_SHADER);

        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        depthDesc.DepthEnable = TRUE;
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(device->CreateDepthStencilState(&depthDesc, &glintDepthState))) throw std::runtime_error("depth state");

        D3D11_BLEND_DESC blendDesc{};
        auto& target = blendDesc.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_SRC_COLOR;
        target.DestBlend = D3D11_BLEND_ONE;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&blendDesc, &glintBlendState))) throw std::runtime_error("blend state");
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[WARN] Armor glint unavailable: ") + e.what());
        glintPixelShader.Reset();
        glintDepthState.Reset();
        glintBlendState.Reset();
    }
    assets = std::move(loaded);
}

void D3D11EntityPass::DrawTextured(D3D11RenderEngine* d3d, Application* state, const EntityScene::Frame& scene)
{
    texturedItems.clear();
    glintItems.clear();
    const bool glintReady = glintPixelShader && glintDepthState && glintBlendState;

    if (const D3D11EntityAssets::Mesh* blockMesh = assets->BlockMesh())
    {
        for (const EntityScene::BlockDraw& block : scene.blocks)
        {
            const D3D11EntityAssets::Skin* texture = assets->BlockTexture(block.stateId);
            if (texture == nullptr) continue;
            EntityScene::TexturedInstance instance{};
            instance.positionYaw = glm::vec4(block.position, 0.0f);
            instance.hurtColor = block.hurtColor;
            instance.partOffset = glm::vec4(0.0f, 0.0f, 0.0f, EntityScene::EntityLight(state, block.position));
            texturedItems.push_back({ blockMesh, texture, blockMesh->indexCount, 0, instance });
        }
    }

    EntityScene::ExpandTextured(state, scene, *assets,
        [&](EntityScene::Batch batch, const EntityScene::MeshBase& mesh, const EntityScene::SkinBase& skin,
            uint32_t indexCount, uint32_t firstIndex, const EntityScene::TexturedInstance& instance)
        {
            const bool glint = batch == EntityScene::Batch::Glint;
            if (glint && !glintReady) return;
            std::vector<TexturedItem>& list = glint ? glintItems : texturedItems;
            if (texturedItems.size() + glintItems.size() >= MAX_TEXTURED_INSTANCES) return;
            list.push_back({ &D3D11EntityAssets::AsD3D(mesh), &D3D11EntityAssets::AsD3D(skin),
                indexCount, firstIndex, instance });
        });
    if (texturedItems.empty() && glintItems.empty()) return;

    // Identical parts of one mesh and skin become one instanced draw: ten
    // zombies' right legs are a single command.
    auto byRun = [](const TexturedItem& a, const TexturedItem& b)
    {
        if (a.mesh != b.mesh) return std::less<const D3D11EntityAssets::Mesh*>()(a.mesh, b.mesh);
        if (a.skin != b.skin) return std::less<const D3D11EntityAssets::Skin*>()(a.skin, b.skin);
        if (a.firstIndex != b.firstIndex) return a.firstIndex < b.firstIndex;
        return a.indexCount < b.indexCount;
    };
    std::sort(texturedItems.begin(), texturedItems.end(), byRun);
    std::sort(glintItems.begin(), glintItems.end(), byRun);

    // One buffer: the normal parts first, the glint copies after them.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(texturedInstanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    auto* dest = static_cast<EntityScene::TexturedInstance*>(mapped.pData);
    for (size_t i = 0; i < texturedItems.size(); ++i) dest[i] = texturedItems[i].instance;
    for (size_t i = 0; i < glintItems.size(); ++i) dest[texturedItems.size() + i] = glintItems[i].instance;
    context->Unmap(texturedInstanceBuffer.Get(), 0);

    context->IASetInputLayout(texturedLayout.Get());
    context->VSSetShader(texturedVertexShader.Get(), nullptr, 0);

    ID3D11Buffer* camera = d3d->GetCameraConstantBuffer();
    context->VSSetConstantBuffers(0, 1, &camera);
    context->PSSetConstantBuffers(0, 1, &camera);
    ID3D11SamplerState* samplers[2] = { assets->GetSampler(), assets->GetGlintSampler() };
    context->PSSetSamplers(0, 2, samplers);

    uint64_t triangles = 0;
    auto drawRuns = [&](const std::vector<TexturedItem>& items, size_t baseInstance, bool glint)
    {
        const D3D11EntityAssets::Mesh* boundMesh = nullptr;
        const D3D11EntityAssets::Skin* boundSkin = nullptr;
        size_t i = 0;
        while (i < items.size())
        {
            const TexturedItem& first = items[i];
            size_t j = i;
            while (j < items.size() && items[j].mesh == first.mesh && items[j].skin == first.skin
                && items[j].firstIndex == first.firstIndex && items[j].indexCount == first.indexCount)
            {
                ++j;
            }

            if (first.mesh != boundMesh)
            {
                ID3D11Buffer* buffers[2] = { first.mesh->vertexBuffer.Get(), texturedInstanceBuffer.Get() };
                UINT strides[2] = { sizeof(EntityJsonModel::Vertex), sizeof(EntityScene::TexturedInstance) };
                UINT offsets[2] = { 0, 0 };
                context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
                context->IASetIndexBuffer(first.mesh->indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
                boundMesh = first.mesh;
            }
            if (first.skin != boundSkin)
            {
                // The glint shader takes the armor (as a coverage mask) at t0 and the glint sheet at t1.
                ID3D11ShaderResourceView* views[2] = { first.skin->view.Get(),
                    glint ? D3D11EntityAssets::AsD3D(*assets->ArmorGlintSkin()).view.Get() : nullptr };
                context->PSSetShaderResources(0, glint ? 2 : 1, views);
                boundSkin = first.skin;
            }
            // StartInstanceLocation offsets the instance-rate slot, so this
            // run reads its own records from the shared buffer.
            context->DrawIndexedInstanced(first.indexCount, static_cast<UINT>(j - i), first.firstIndex, 0,
                static_cast<UINT>(baseInstance + i));
            triangles += static_cast<uint64_t>(first.indexCount / 3) * (j - i);
            i = j;
        }
    };

    if (!texturedItems.empty())
    {
        context->PSSetShader(texturedPixelShader.Get(), nullptr, 0);
        drawRuns(texturedItems, 0, false);
    }
    if (!glintItems.empty())
    {
        context->PSSetShader(glintPixelShader.Get(), nullptr, 0);
        context->OMSetDepthStencilState(glintDepthState.Get(), 0);
        context->OMSetBlendState(glintBlendState.Get(), nullptr, 0xFFFFFFFF);
        drawRuns(glintItems, texturedItems.size(), true);
        context->OMSetDepthStencilState(depthState.Get(), 0);
        context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    }
    if (state->debugHudVisible) state->trianglesDrawn[Application::TRIANGLES_ENTITY] += triangles;

    ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
    context->PSSetShaderResources(0, 2, none);
}

void D3D11EntityPass::Record(FrameContext& frame)
{
    if (!vertexShader || !pixelShader || !inputLayout) return;

    D3D11RenderEngine* d3d = frame.engine->GetD3D11();
    if (!d3d) return;

    Application* state = frame.state;

    // Culling, poses and the per-shape instance lists come from the layer
    // shared with the Vulkan backend. viewProj/proj carry D3D11's un-flipped Y
    // (see D3D11RenderEngine::SubmitFrame); the frustum test is symmetric in Y.
    static NoModelAssets noModels;
    static EntityScene::Frame scene; // Reused so its lists keep their capacity.
    EntityScene::Assets& sceneAssets = assets ? static_cast<EntityScene::Assets&>(*assets) : noModels;
    if (assets) assets->UploadFinishedSkins(state);
    EntityScene::Collect(state, frame.scene->view, frame.scene->proj, frame.scene->cameraPosition, sceneAssets, scene);

    // Block-shaped entities (falling blocks, TNT) draw textured when their
    // block has a usable texture; the rest fall back to a plain unit cube.
    for (const EntityScene::BlockDraw& block : scene.blocks)
    {
        if (assets && assets->BlockMesh() != nullptr && assets->BlockTexture(block.stateId) != nullptr) continue;
        GPUEntityInstance gpu{};
        gpu.positionYaw = glm::vec4(block.position, 0.0f);
        gpu.halfExtents = glm::vec4(0.49f, 0.49f, 0.49f, 0.0f);
        gpu.color = glm::vec4(0.55f, 0.55f, 0.55f, 1.0f);
        gpu.hurtColor = block.hurtColor;
        scene.flat[EntityScene::FLAT_CUBE].push_back(gpu);
    }

    const std::array<std::vector<GPUEntityInstance>, SHAPE_COUNT>& instances = scene.flat;

    bool any = false;
    for (const auto& list : instances) any = any || !list.empty();

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(inputLayout.Get());
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);

    ID3D11Buffer* camera = d3d->GetCameraConstantBuffer();
    context->VSSetConstantBuffers(0, 1, &camera);

    context->RSSetState(rasterizer.Get());
    context->OMSetDepthStencilState(depthState.Get(), 0);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    for (uint32_t shapeIndex = 0; shapeIndex < SHAPE_COUNT && any; ++shapeIndex)
    {
        const std::vector<GPUEntityInstance>& list = instances[shapeIndex];
        if (list.empty() || list.size() > MAX_VISIBLE_ENTITIES) continue;
        ShapeResources& shape = shapes[shapeIndex];

        D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(list.size() * sizeof(GPUEntityInstance)), 1, 1 };
        context->UpdateSubresource(shape.instanceBuffer.Get(), 0, &box, list.data(), 0, 0);

        ID3D11ShaderResourceView* view = shape.instanceView.Get();
        context->VSSetShaderResources(0, 1, &view);

        ID3D11Buffer* vertexBuffer = shape.vertexBuffer.Get();
        UINT stride = sizeof(EntityMeshVertex);
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context->IASetIndexBuffer(shape.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        context->DrawIndexedInstanced(shape.indexCount, static_cast<UINT>(list.size()), 0, 0, 0);

        if (state->debugHudVisible)
        {
            state->trianglesDrawn[Application::TRIANGLES_ENTITY] +=
                static_cast<uint64_t>(shape.indexCount / 3) * list.size();
        }
    }

    // Leave t0 free for the passes that follow.
    ID3D11ShaderResourceView* none = nullptr;
    context->VSSetShaderResources(0, 1, &none);

    if (assets) DrawTextured(d3d, state, scene);

    // Held items and the hand borrow our assets (the local skin); items go last
    // so they draw over the bodies they ride on.
    D3D11ItemRenderer* items = state->d3dItemRenderer;
    if (items != nullptr && assets)
    {
        items->SetEntityAssets(assets.get());
        registeredItems = items;
        EntityScene::CollectHeldItems(scene, heldItems);
        for (const EntityScene::HeldItemDraw& held : heldItems)
        {
            items->DrawItem(*held.item, held.context, held.placement, frame.viewProj, glm::mat4(1.0f), 0.0f, held.glint);
        }
    }
}

void D3D11EntityPass::Destroy()
{
    if (registeredItems != nullptr) registeredItems->SetEntityAssets(nullptr);
    registeredItems = nullptr;
    heldItems.clear();
    for (ShapeResources& shape : shapes) shape = ShapeResources{};
    if (assets) assets->Release();
    assets.reset();
    texturedVertexShader.Reset();
    texturedPixelShader.Reset();
    texturedLayout.Reset();
    texturedInstanceBuffer.Reset();
    texturedItems.clear();
    glintItems.clear();
    glintPixelShader.Reset();
    glintDepthState.Reset();
    glintBlendState.Reset();
    vertexShader.Reset();
    pixelShader.Reset();
    inputLayout.Reset();
    depthState.Reset();
    rasterizer.Reset();
}

} // namespace Volcano::Engine::D3D11
