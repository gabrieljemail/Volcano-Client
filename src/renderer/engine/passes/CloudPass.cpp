#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <variant>
#include <stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include "CloudPass.hpp"
#include "../ShaderLibrary.hpp"
#include "../../VulkanInit.hpp"
#include "../../../Config.hpp"
#include "../../../GlobalState.hpp"
#include "../../../Logger.hpp"
#include "../../../resources/ImageLoad.hpp"

namespace Volcano::Engine::Passes {

namespace {

constexpr const char* CLOUD_TEXTURE = "assets/minecraft/textures/environment/clouds.png"; // Logical pack path.

// Vanilla's cloud base is 192.33 blocks up in the overworld.
constexpr uint32_t DEFAULT_CLOUD_HEIGHT = 192;

// Face brightness, top lightest.
constexpr float SHADE_TOP = 1.0f;
constexpr float SHADE_BOTTOM = 0.7f;
constexpr float SHADE_NORTH_SOUTH = 0.8f;
constexpr float SHADE_EAST_WEST = 0.9f;

// Must match clouds.vert's Push block.
struct Push {
    glm::mat4 mvp;
    glm::vec4 color;
    glm::vec4 offset;
};

} // namespace

void CloudPass::Create(const ShaderLibrary& shaders)
{
    if (!LoadCells()) return;

    try
    {
        for (Slot& slot : slots)
        {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = sizeof(Vertex) * MAX_VERTICES;
            bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &slot.buffer, &slot.allocation, &info) != VK_SUCCESS)
            {
                slot.buffer = VK_NULL_HANDLE;
                throw std::runtime_error("[ERROR] Failed to create cloud vertex buffer.");
            }
            slot.mapped = static_cast<Vertex*>(info.pMappedData);
        }
        CreatePipeline(shaders);
    }
    catch (...)
    {
        Destroy();
        throw;
    }
    Log::Info("Cloud pass active.");
}

void CloudPass::ReloadShaders(const ShaderLibrary& shaders)
{
    if (cells.empty()) return;
    DestroyPipeline();
    CreatePipeline(shaders);
}

void CloudPass::ReloadTextures(const ShaderLibrary& shaders)
{
    if (cells.empty())
    {
        // The first load failed, so nothing was created; try the whole pass again.
        Destroy();
        Create(shaders);
        return;
    }

    if (!LoadCells()) return;
    for (Slot& slot : slots) slot.mode = -1; // Force every mesh to rebuild from the new cells.
}

bool CloudPass::LoadCells()
{
    int width = 0, height = 0;
    stbi_uc* pixels = LoadImageRGBA(CLOUD_TEXTURE, width, height);
    if (pixels == nullptr || width != TEXTURE_SIZE || height != TEXTURE_SIZE)
    {
        Log::Error(std::string("[ERROR] Clouds: couldn't use ") + CLOUD_TEXTURE + " (missing, or not 256x256).");
        if (pixels != nullptr) stbi_image_free(pixels);
        return false;
    }

    cells.assign(static_cast<size_t>(TEXTURE_SIZE) * TEXTURE_SIZE, false);
    for (size_t i = 0; i < cells.size(); i++) cells[i] = pixels[i * 4 + 3] > 0;
    stbi_image_free(pixels);
    return true;
}

bool CloudPass::CellAt(int x, int z) const
{
    int px = ((x % TEXTURE_SIZE) + TEXTURE_SIZE) % TEXTURE_SIZE;
    int pz = ((z % TEXTURE_SIZE) + TEXTURE_SIZE) % TEXTURE_SIZE;
    return cells[static_cast<size_t>(pz) * TEXTURE_SIZE + px];
}

void CloudPass::BuildMesh(Slot& slot, int centerX, int centerZ, int mode)
{
    uint32_t count = 0;

    // Corners must be counter-clockwise seen from outside (the terrain's
    // front-face convention); two triangles per quad.
    auto quad = [&](float shade, const float (&c)[4][3])
    {
        if (count + 6 > MAX_VERTICES) return;
        static const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int index : order) slot.mapped[count++] = { c[index][0], c[index][1], c[index][2], shade };
    };

    const bool fancy = mode == 2;
    const float y0 = 0.0f;
    const float y1 = fancy ? THICKNESS : 0.0f;

    for (int dz = -RADIUS_CELLS; dz < RADIUS_CELLS; dz++)
    {
        for (int dx = -RADIUS_CELLS; dx < RADIUS_CELLS; dx++)
        {
            int cellX = centerX + dx, cellZ = centerZ + dz;
            if (!CellAt(cellX, cellZ)) continue;

            float x0 = dx * CELL_SIZE, x1 = x0 + CELL_SIZE;
            float z0 = dz * CELL_SIZE, z1 = z0 + CELL_SIZE;

            quad(SHADE_TOP, { { x0, y1, z0 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 } });
            quad(SHADE_BOTTOM, { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 } });
            if (!fancy) continue;

            if (!CellAt(cellX + 1, cellZ))
                quad(SHADE_EAST_WEST, { { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 } });
            if (!CellAt(cellX - 1, cellZ))
                quad(SHADE_EAST_WEST, { { x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 } });
            if (!CellAt(cellX, cellZ + 1))
                quad(SHADE_NORTH_SOUTH, { { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } });
            if (!CellAt(cellX, cellZ - 1))
                quad(SHADE_NORTH_SOUTH, { { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y0, z0 } });
        }
    }

    slot.vertexCount = count;
    slot.centerX = centerX;
    slot.centerZ = centerZ;
    slot.mode = mode;
}

void CloudPass::Record(FrameContext& frame)
{
    if (pipeline == VK_NULL_HANDLE) return;
    const SceneView& scene = *frame.scene;
    if (scene.menuBackground) return;

    Config* config = frame.state->config;
    int mode = static_cast<int>(std::min<uint32_t>(std::get<uint32_t>(config->Get("Graphics.Clouds", uint32_t{2})), 2));
    if (mode == 0) return;
    float height = static_cast<float>(std::get<uint32_t>(config->Get("Graphics.CloudHeight", DEFAULT_CLOUD_HEIGHT)));

    // The grid is anchored to the drifting cloud field, so its centre cell is
    // whichever one the camera is over now.
    float drift = scene.time * DRIFT_BLOCKS_PER_SECOND;
    glm::vec3 camera = scene.cameraPosition;
    int centerX = static_cast<int>(std::floor((camera.x - drift) / CELL_SIZE));
    int centerZ = static_cast<int>(std::floor(camera.z / CELL_SIZE));

    Slot& slot = slots[frame.frameIndex % FRAME_SLOTS];
    if (slot.mode != mode || slot.centerX != centerX || slot.centerZ != centerZ) BuildMesh(slot, centerX, centerZ, mode);
    if (slot.vertexCount == 0) return;

    glm::vec3 offset(centerX * CELL_SIZE + drift - camera.x, height - camera.y, centerZ * CELL_SIZE - camera.z);

    // White by day, a dim blue-grey at night (linear, like the sky colors).
    float daylight = glm::smoothstep(0.15f, 0.65f, scene.dayBrightness);
    glm::vec3 color = glm::mix(glm::vec3(0.012f, 0.014f, 0.03f), glm::vec3(1.0f), daylight);

    Push push{};
    push.mvp = scene.proj * glm::mat4(glm::mat3(scene.view)) * glm::translate(glm::mat4(1.0f), offset);
    push.color = glm::vec4(color, 0.8f);
    push.offset = glm::vec4(offset, RADIUS_CELLS * CELL_SIZE);

    VkCommandBuffer cmd = frame.commandBuffer;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &slot.buffer, &vertexOffset);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
    vkCmdDraw(cmd, slot.vertexCount, 1, 0, 0);
}

void CloudPass::CreatePipeline(const ShaderLibrary& shaders)
{
    VkDevice dev = GetDevice();
    VkShaderModule vertModule = CreateShaderModule(shaders, VERTEX_SHADER);
    VkShaderModule fragModule = VK_NULL_HANDLE;
    try
    {
        fragModule = CreateShaderModule(shaders, FRAGMENT_SHADER);
    }
    catch (...)
    {
        vkDestroyShaderModule(dev, vertModule, nullptr);
        throw;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[2]{};
    attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, x) };
    attributes[1] = { 1, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, shade) };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Behind terrain and entities, but never in front of them in the depth
    // buffer the Hi-Z pyramid is built from.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    VkResult result = vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout);
    if (result == VK_SUCCESS)
    {
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        result = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        if (result != VK_SUCCESS) pipeline = VK_NULL_HANDLE;
    }
    else
    {
        pipelineLayout = VK_NULL_HANDLE;
    }

    vkDestroyShaderModule(dev, fragModule, nullptr);
    vkDestroyShaderModule(dev, vertModule, nullptr);

    if (result != VK_SUCCESS)
    {
        DestroyPipeline();
        throw std::runtime_error("[ERROR] Failed to create cloud pipeline.");
    }
}

void CloudPass::DestroyPipeline()
{
    VkDevice dev = GetDevice();
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
}

void CloudPass::Destroy()
{
    DestroyPipeline();
    for (Slot& slot : slots)
    {
        if (slot.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, slot.buffer, slot.allocation);
        slot = Slot{};
    }
}

} // namespace Volcano::Engine::Passes
