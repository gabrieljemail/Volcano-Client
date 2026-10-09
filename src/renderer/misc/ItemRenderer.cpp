#include "ItemRenderer.hpp"
#include <array>
#include <cstring>
#include <stdexcept>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "ItemModel.hpp"
#include "../VulkanInit.hpp"
#include "../../Logger.hpp"

namespace Volcano {

namespace {

constexpr VkFormat ICON_FORMAT = VK_FORMAT_R8G8B8A8_UNORM; // Same gamma-space convention as the texture array.

// Must match item.vert's PushConstants block: 128 bytes, the guaranteed minimum.
struct ItemPushConstants {
    glm::mat4 mvp;
    glm::vec4 normalRow0;
    glm::vec4 normalRow1;
    glm::vec4 normalRow2;
    glm::vec4 params; // x: 1 = flat item.
};
static_assert(sizeof(ItemPushConstants) == 128, "ItemPushConstants must fit the 128-byte push constant minimum");

} // namespace

void ItemRenderer::Init(const Engine::ShaderLibrary& shaders)
{
    if (initialized) return;
    CreateRenderPass();
    CreateDepthBuffer();
    CreatePipeline(shaders);
    initialized = true;
}

void ItemRenderer::Shutdown()
{
    if (!initialized) return;
    vkDeviceWaitIdle(GetDevice());

    for (auto& [name, mesh] : meshes)
    {
        if (mesh.vertices.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, mesh.vertices.buffer, mesh.vertices.allocation);
        if (mesh.indices.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, mesh.indices.buffer, mesh.indices.allocation);
    }
    meshes.clear();

    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(GetDevice(), pipelineLayout, nullptr);
    if (depthView != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), depthView, nullptr);
    if (depthImage != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, depthImage, depthAllocation);
    if (renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(GetDevice(), renderPass, nullptr);
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    depthView = VK_NULL_HANDLE;
    depthImage = VK_NULL_HANDLE;
    renderPass = VK_NULL_HANDLE;
    initialized = false;
}

void ItemRenderer::CreateRenderPass()
{
    std::array<VkAttachmentDescription, 2> attachments{};
    attachments[0].format = ICON_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format = depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // Wait for the previous icon's depth use, and make the colour result
    // visible to later fragment-shader reads.
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    if (vkCreateRenderPass(GetDevice(), &info, nullptr, &renderPass) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] ItemRenderer: failed to create the icon render pass.");
    }
}

void ItemRenderer::CreateDepthBuffer()
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { GUI_ICON_SIZE, GUI_ICON_SIZE, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = depthFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(vmaAllocator, &imageInfo, &allocInfo, &depthImage, &depthAllocation, nullptr) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] ItemRenderer: failed to create the icon depth buffer.");
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &depthView) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] ItemRenderer: failed to create the icon depth view.");
    }
}

void ItemRenderer::CreatePipeline(const Engine::ShaderLibrary& shaders)
{
    VkShaderModule vertModule = CreateShaderModule(shaders, "item.vert");
    VkShaderModule fragModule = VK_NULL_HANDLE;
    try
    {
        fragModule = CreateShaderModule(shaders, "item.frag");
    }
    catch (...)
    {
        vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
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

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(ItemModelVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attributes{};
    attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(ItemModelVertex, position)) };
    attributes[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(ItemModelVertex, normal)) };
    attributes[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(ItemModelVertex, uv)) };
    attributes[3] = { 3, 0, VK_FORMAT_R32_SFLOAT, static_cast<uint32_t>(offsetof(ItemModelVertex, layer)) };

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

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

    // Same winding convention as the entity pipelines: the projection's Y is
    // pre-flipped (see RenderGuiIcon), meshes are CCW from outside.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ItemPushConstants) };
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &textureSetLayout; // Set 0 = the block/item texture array.
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(GetDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
    {
        vkDestroyShaderModule(GetDevice(), fragModule, nullptr);
        vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
        throw std::runtime_error("[ERROR] ItemRenderer: failed to create the pipeline layout.");
    }

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
    VkResult result = vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

    vkDestroyShaderModule(GetDevice(), fragModule, nullptr);
    vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
    if (result != VK_SUCCESS)
    {
        pipeline = VK_NULL_HANDLE;
        throw std::runtime_error("[ERROR] ItemRenderer: failed to create the graphics pipeline.");
    }
}

ItemRenderer::GpuBuffer ItemRenderer::UploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    // Host-visible and persistently mapped: these meshes are tiny and drawn
    // rarely, so a staging copy would only add code.
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    GpuBuffer result;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &info) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] ItemRenderer: failed to allocate a mesh buffer.");
    }
    std::memcpy(info.pMappedData, data, static_cast<size_t>(size));
    vmaFlushAllocation(vmaAllocator, result.allocation, 0, VK_WHOLE_SIZE);
    return result;
}

const ItemRenderer::GpuMesh* ItemRenderer::MeshFor(const std::string& itemName, const ItemModel& model)
{
    if (auto it = meshes.find(itemName); it != meshes.end()) return &it->second;

    GpuMesh mesh;
    mesh.vertices = UploadBuffer(model.GetVertices().data(), model.GetVertices().size() * sizeof(ItemModelVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    mesh.indices = UploadBuffer(model.GetIndices().data(), model.GetIndices().size() * sizeof(uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    mesh.indexCount = static_cast<uint32_t>(model.GetIndices().size());
    return &(meshes[itemName] = mesh);
}

bool ItemRenderer::RenderGuiIcon(const std::string& itemName, IconImage& out)
{
    out = IconImage{};
    if (!initialized || textures == nullptr) return false;

    const ItemModel* model = ItemModel::ForItem(itemName, *textures);
    if (model == nullptr) return false;
    const GpuMesh* mesh = MeshFor(itemName, *model);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { GUI_ICON_SIZE, GUI_ICON_SIZE, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = ICON_FORMAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(vmaAllocator, &imageInfo, &allocInfo, &out.image, &out.allocation, nullptr) != VK_SUCCESS)
    {
        out = IconImage{};
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = out.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = ICON_FORMAT;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &out.view) != VK_SUCCESS)
    {
        vmaDestroyImage(vmaAllocator, out.image, out.allocation);
        out = IconImage{};
        return false;
    }

    VkImageView attachments[] = { out.view, depthView };
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = GUI_ICON_SIZE;
    framebufferInfo.height = GUI_ICON_SIZE;
    framebufferInfo.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(GetDevice(), &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS)
    {
        vkDestroyImageView(GetDevice(), out.view, nullptr);
        vmaDestroyImage(vmaAllocator, out.image, out.allocation);
        out = IconImage{};
        return false;
    }

    // The model's own "gui" transform, viewed down -Z through a unit-sized
    // orthographic window (one item = one 16px vanilla slot). Y is flipped
    // for Vulkan's clip space, like the world projection.
    // Plus a quarter turn about the vertical axis: the display transform
    // alone came out 90 degrees to the right of vanilla's icon.
    glm::mat4 modelMatrix = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f))
        * model->DisplayMatrix("gui");
    glm::mat4 projection = glm::orthoRH_ZO(-0.5f, 0.5f, -0.5f, 0.5f, -2.0f, 2.0f);
    projection[1][1] *= -1.0f;

    glm::mat3 normalMatrix(modelMatrix);
    ItemPushConstants push{};
    push.mvp = projection * modelMatrix;
    push.normalRow0 = glm::vec4(normalMatrix[0][0], normalMatrix[1][0], normalMatrix[2][0], 0.0f);
    push.normalRow1 = glm::vec4(normalMatrix[0][1], normalMatrix[1][1], normalMatrix[2][1], 0.0f);
    push.normalRow2 = glm::vec4(normalMatrix[0][2], normalMatrix[1][2], normalMatrix[2][2], 0.0f);
    push.params = glm::vec4(model->IsFlat() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

    VkCommandBuffer cmd = BeginOneShotCommands();

    std::array<VkClearValue, 2> clears{};
    clears[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clears[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = renderPass;
    passInfo.framebuffer = framebuffer;
    passInfo.renderArea = { { 0, 0 }, { GUI_ICON_SIZE, GUI_ICON_SIZE } };
    passInfo.clearValueCount = static_cast<uint32_t>(clears.size());
    passInfo.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(GUI_ICON_SIZE), static_cast<float>(GUI_ICON_SIZE), 0.0f, 1.0f };
    VkRect2D scissor{ { 0, 0 }, { GUI_ICON_SIZE, GUI_ICON_SIZE } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet texturesSet = textureSet; // The global texture array descriptor (VulkanInit).
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &texturesSet, 0, nullptr);
    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertices.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(cmd, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);

    vkCmdEndRenderPass(cmd);
    EndOneShotCommands(cmd); // Submits and waits.

    vkDestroyFramebuffer(GetDevice(), framebuffer, nullptr);
    return true;
}

} // namespace Volcano
