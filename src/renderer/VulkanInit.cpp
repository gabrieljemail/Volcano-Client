#include <thread>
#include <memory>
#include <vector>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define VMA_IMPLEMENTATION
#include "GlobalState.hpp"
#include "Logger.hpp"
#include "VulkanInit.hpp"
#include "RenderThread.hpp"
#include "../Helpers.hpp"
#include "models/Mesh.hpp"
#include "models/CameraUBO.hpp"
#include "models/PackedVertex.hpp"
#include "terrain/ChunkMesher.hpp"
#include "entity/EntityRenderer.hpp"
#include "misc/MiscVertex.hpp"
#include "misc/NonCubicMesher.hpp"

using namespace std;

// Set once in Init(). Kept as a plain module-local pointer rather than the
// GLFW window user pointer, since InputHandler already claims that slot for
// its own key/cursor callbacks.
static GlobalState* g_globalState = nullptr;

// Current window mode and the windowed geometry to restore when leaving
// fullscreen (captured right before switching away from Windowed).
static WindowMode g_windowMode = WindowMode::Windowed;
static int g_windowedX = 0, g_windowedY = 0;
static int g_windowedWidth = WINDOW_WIDTH, g_windowedHeight = WINDOW_HEIGHT;

static void FramebufferSizeCallback(GLFWwindow* /*win*/, int width, int height)
{
    if (!g_globalState) return;
    g_globalState->pendingFramebufferWidth = width;
    g_globalState->pendingFramebufferHeight = height;
    g_globalState->framebufferResized = true;
}

#ifdef _WIN32
// Set once the render thread has presented at least one frame — see
// NotifyFramePresented(). GLFW_FOCUSED can already read true immediately
// after glfwCreateWindow() even though Windows hasn't actually shown/
// composited the window yet, so a focus-gained event (or an eager check at
// InputHandler construction — since removed) can fire before the window is
// really up. Locking the cursor at that point clips it to a window the
// desktop hasn't painted, which looks like a visible cursor trapped in a
// "ghost" window until the user clicks it. Withholding the lock until a
// frame has genuinely been presented fixes that ordering.
static bool g_firstFramePresented = false;

static void WindowFocusCallback(GLFWwindow* win, int focused)
{
    if (focused)
    {
        // Don't steal the cursor back from ImGui while a Screen (connect
        // screen, pause menu, etc.) is open — GUIController itself owns
        // cursor mode in that case (see OpenScreen/CloseScreen).
        if (g_firstFramePresented && !GUIController::IsScreenOpen())
        {
            glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }
    else
    {
        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}
#endif

void NotifyFramePresented()
{
#ifdef _WIN32
    if (g_firstFramePresented) return;
    g_firstFramePresented = true;

    // The window may already have (real) focus by the time this first
    // frame lands, in which case no further WindowFocusCallback will ever
    // fire to lock the cursor — so apply the same check here once, now
    // that the window is genuinely on screen.
    if (window && glfwGetWindowAttrib(window, GLFW_FOCUSED) && !GUIController::IsScreenOpen())
    {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }
#endif
}

// Helper to create a ShaderModule from raw SPIR-V bytecode
VkShaderModule CreateShaderModule(VkDevice dev, const std::vector<char>& code)
{
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(dev, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module!");
    }
    return shaderModule;
}

void CreateGraphicsPipeline()
{
    VkDevice dev = GetDevice();

    // 1. LOAD COMPILED SPIR-V BINARIES
    std::vector<char> vertShaderCode = ReadFile("resources/shaders/terrain.vert.spv");
    std::vector<char> fragShaderCode = ReadFile("resources/shaders/terrain.frag.spv");

    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        throw std::runtime_error("[ERROR] Failed to read shader files: resources/shaders/terrain.vert.spv or resources/shaders/terrain.frag.spv");
    }

    VkShaderModule vertShaderModule = CreateShaderModule(dev, vertShaderCode);
    VkShaderModule fragShaderModule = CreateShaderModule(dev, fragShaderCode);

    // 2. SET UP SHADER STAGES
    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

    // 3. VERTEX INPUT STATE
    // The terrain pipeline's only vertex format is the packed uvec3 chunk mesh
    // format (see PackedVertex.hpp / terrain.vert), not the generic float Vertex.
    auto bindingDescription = Volcano::PackedVertex::getBindingDescription();
    auto attributeDescriptions = Volcano::PackedVertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    // 4. INPUT ASSEMBLY
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // 5. VIEWPORT & SCISSOR (DYNAMIC STATE SETUP)
    // Making viewport and scissor dynamic means you don't need to rebuild the entire
    // pipeline when resizing the window!
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
    dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicStateInfo.pDynamicStates = dynamicStates.data();

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // 6. RASTERIZATION STATE
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    // No backface culling for terrain. This used to be VK_CULL_MODE_BACK_BIT
    // with VK_FRONT_FACE_CLOCKWISE, on the theory that ChunkMesher's greedy
    // quads are consistently wound CCW-from-outside in world space (see
    // GreedyMeshAxis's positiveCorners) and RenderThread's p[1][1] *= -1
    // projection flip (needed to land the image right-side-up) inverts that
    // to CW in framebuffer space uniformly for every face. In practice that
    // held for wall faces (X/Z-swept) but NOT for floor/ceiling faces
    // (Y-swept) — after correcting for the wall case, floors still rendered
    // inside-out (visible from below, culled from above, letting you see
    // through solid ground to bedrock) — a per-axis asymmetry the theory
    // above says shouldn't exist, and repeated attempts to hand-derive the
    // actual rule didn't resolve it. Rather than continue guessing, terrain
    // just draws both sides now: every face renders regardless of view
    // angle, so whichever winding is nearer the camera wins the depth test
    // and looks correct either way. This also means a player wedged inside
    // a solid block sees that block's own surface (like any other wall)
    // instead of every face flipping to show the far side of the world —
    // no more "wallhack" view from getting stuck. Costs roughly double the
    // fragment work versus perfectly-tuned culling; not worth chasing
    // further on an already-60fps-capped integrated GPU.
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // 7. SINGLE-PASS SAMPLING (MULTISAMPLING)
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // DEPTH AND STENCIL STATE
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // 8. COLOR BLENDING
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                          VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT |
                                          VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE; // Direct write for opaque geometry.

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // 9. PIPELINE LAYOUT (Push Constants & Descriptors)
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    pipelineLayoutInfo.setLayoutCount = 2;
    VkDescriptorSetLayout setLayouts[] = { cameraSetLayout, textureSetLayout };
    pipelineLayoutInfo.pSetLayouts = setLayouts;

    if (vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create pipeline layout.");
    }

    cout << "[INFO] Compiling shaders..." << endl;

    // 10. CREATE GRAPHICS PIPELINE
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicStateInfo;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create graphics pipeline!");
    }

    // Wireframe variant of the same pipeline (F3, see RenderThread) — a
    // debug tool for seeing where mesh geometry actually is versus what the
    // fill-mode result implies, e.g. telling "this face is missing
    // entirely" apart from "this face exists but lost the depth/winding
    // test" (which polygon fill alone can't distinguish). Shares every
    // other piece of state with the pipeline above — only polygonMode
    // differs — so it's built from the exact same CreateInfo, not a
    // parallel hand-copied one that could quietly drift out of sync.
    VkPipelineRasterizationStateCreateInfo wireframeRasterizer = rasterizer;
    wireframeRasterizer.polygonMode = VK_POLYGON_MODE_LINE;
    wireframeRasterizer.cullMode = VK_CULL_MODE_NONE;
    wireframeRasterizer.lineWidth = 1.0f; // >1.0 needs the wideLines feature, which isn't requested/guaranteed enabled.

    VkGraphicsPipelineCreateInfo wireframePipelineInfo = pipelineInfo;
    wireframePipelineInfo.pRasterizationState = &wireframeRasterizer;

    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &wireframePipelineInfo, nullptr, &wireframePipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create wireframe graphics pipeline!");
    }

    // 11. CLEANUP TEMPORARY SHADER MODULES
    vkDestroyShaderModule(dev, fragShaderModule, nullptr);
    vkDestroyShaderModule(dev, vertShaderModule, nullptr);

    Volcano::Log::Info("[INFO] Graphics pipeline successfully initialized.");
}

// Second pass for everything that isn't a fully opaque cube: transparent
// full cubes (glass, slime, ice, ...), partial-volume boxes (slabs,
// carpets, ...), and cross-shaped plants (grass, flowers, ...) — see
// NonCubicMesher. Reuses the terrain pass's pipelineLayout (identical push
// constant + descriptor set layout signature: camera UBO + texture array),
// so only a new VkPipeline with different fixed-function state is needed:
//  - No backface culling: this pass covers shapes meant to be seen from
//    both sides (a cross billboard, the inner face of a glass pane) rather
//    than closed solids, so the task's "shouldn't contribute to backface
//    culling" is satisfied by disabling culling for the pass entirely.
//  - Alpha blending on, depth WRITE off (but depth TEST still on): lets
//    partially transparent geometry blend over whatever the opaque pass
//    already wrote, without one piece of this pass's own geometry
//    occluding another based on draw order alone.
void CreateNonCubicPipeline()
{
    VkDevice dev = GetDevice();

    std::vector<char> vertShaderCode = ReadFile("resources/shaders/misc.vert.spv");
    std::vector<char> fragShaderCode = ReadFile("resources/shaders/misc.frag.spv");

    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        throw std::runtime_error("[ERROR] Failed to read shader files: resources/shaders/misc.vert.spv or resources/shaders/misc.frag.spv");
    }

    VkShaderModule vertShaderModule = CreateShaderModule(dev, vertShaderCode);
    VkShaderModule fragShaderModule = CreateShaderModule(dev, fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

    auto bindingDescription = Volcano::MiscVertex::getBindingDescription();
    auto attributeDescriptions = Volcano::MiscVertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
    dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicStateInfo.pDynamicStates = dynamicStates.data();

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // See this function's own comment on why.
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE; // See this function's own comment on why.
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                          VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT |
                                          VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicStateInfo;
    pipelineInfo.layout = pipelineLayout; // Shared with the opaque terrain pipeline — see this function's own comment.
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &nonCubicPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create non-cubic graphics pipeline!");
    }

    vkDestroyShaderModule(dev, fragShaderModule, nullptr);
    vkDestroyShaderModule(dev, vertShaderModule, nullptr);

    Volcano::Log::Info("[INFO] Non-cubic graphics pipeline successfully initialized.");
}

// Create a buffer with VMA.
AllocatedBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool isUMA)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = isUMA ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : 0;

    AllocatedBuffer result{};
    vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &result.info);
    allocatedBuffers.push_back(result);
    return result;
}

// Depth buffer, swapchain image views, and framebuffers are all sized off
// the swapchain extent, so both Init() and RecreateSwapchain() (after a
// resize / F11 mode change) rebuild them the same way via these helpers.
static void CreateDepthResources()
{
    depthFormat = VK_FORMAT_D32_SFLOAT; // We could find supported format, but D32 is widely supported

    VkImageCreateInfo depthImageInfo{};
    depthImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
    depthImageInfo.extent.width = swapchainExtent.width;
    depthImageInfo.extent.height = swapchainExtent.height;
    depthImageInfo.extent.depth = 1;
    depthImageInfo.mipLevels = 1;
    depthImageInfo.arrayLayers = 1;
    depthImageInfo.format = depthFormat;
    depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo depthAllocInfo{};
    depthAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    depthAllocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    if (vmaCreateImage(vmaAllocator, &depthImageInfo, &depthAllocInfo, &depthImage, &depthImageAllocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create depth image!");
    }

    VkImageViewCreateInfo depthViewInfo{};
    depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewInfo.image = depthImage;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = depthFormat;
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.subresourceRange.baseMipLevel = 0;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.baseArrayLayer = 0;
    depthViewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device.device, &depthViewInfo, nullptr, &depthImageView) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create depth image view!");
    }
}

static void CreateSwapchainImageViews()
{
    uint32_t imageCount = swapchain.image_count;
    imageViews.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchain.get_images().value()[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchain.image_format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        VkResult createViewResult = vkCreateImageView(device.device, &viewInfo, nullptr, &imageViews[i]);
        if (createViewResult != VK_SUCCESS)
        {
            throw runtime_error("[ERROR] Failed to create swapchain image view.");
        }
    }
}

static void CreateFramebuffers()
{
    uint32_t imageCount = swapchain.image_count;
    framebuffers.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++)
    {
        VkImageView fbAttachments[] = {
            imageViews[i],
            depthImageView
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = fbAttachments;
        framebufferInfo.width = swapchain.extent.width;
        framebufferInfo.height = swapchain.extent.height;
        framebufferInfo.layers = 1;

        VkResult createFramebufferResult = vkCreateFramebuffer(device.device, &framebufferInfo, nullptr, &framebuffers[i]);
        if (createFramebufferResult != VK_SUCCESS)
        {
            throw std::runtime_error("[ERROR] Failed to create framebuffer.");
        }
    }
}

// Main function.
void Init(GlobalState* state)
{
    // On Linux, try to use Wayland instead of X11.
    #ifdef __linux__

        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);

    #endif

    // Initialize GLFW.
    if (!glfwInit())
    {
        cerr << "[ERROR] Failed to initialize GLFW." << endl;
        Cleanup();
        exit(5);
        return;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Prevent GLFW from creating an OpenGL context.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE); // Show system window decorations.

    // Create the window.
    window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, APP_NAME, nullptr, nullptr);

    if (!window)
    {
        cerr << "[ERROR] Failed to create window." << endl;
        glfwTerminate();
        Cleanup();
        exit(6);
        return;
    }
    state->window = window;
    g_globalState = state;
    glfwGetWindowPos(window, &g_windowedX, &g_windowedY);
    glfwGetWindowSize(window, &g_windowedWidth, &g_windowedHeight);

    // Framebuffer-size changes (user resize, and F11's glfwSetWindowMonitor
    // calls) land here on the main thread; the render thread picks the flag
    // up between frames and recreates the swapchain there, since that's the
    // only thread allowed to touch it.
    glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);

    #ifdef _WIN32
        // Windows-only fallback: on some setups (seen on Intel Gen9 HD 530,
        // driver 31.0.101.2115) the window doesn't actually have input focus
        // the moment it's created, so the GLFW_CURSOR_DISABLED set below
        // never takes effect until the user manually minimizes/restores the
        // window. Re-apply the cursor lock whenever the window actually
        // gains focus, and release it on focus loss so a captured cursor
        // doesn't trap the user's mouse/Alt+Tab when the game is unfocused.
        glfwSetWindowFocusCallback(window, WindowFocusCallback);
    #endif

    // Make an instance.
    vkb::InstanceBuilder instanceBuilder;
    auto instanceReturn = instanceBuilder
        .set_app_name(APP_NAME)
        .set_app_version(APP_VERSION)
        .set_engine_name(ENGINE_NAME)
        .set_engine_version(ENGINE_VERSION)
        .require_api_version(1,1,0)
        // .request_validation_layers()
        .build();

    if (!instanceReturn)
    {
        cerr << "[ERROR] Failed to create Vulkan instance: " << instanceReturn.error().message() << endl;
        Cleanup();
        exit(2);
        return;
    }
    instance = instanceReturn.value();

    // Create the surface.
    VkResult error = glfwCreateWindowSurface(instance.instance, window, nullptr, &surface);
    if (error != VK_SUCCESS)
    {
        cerr << "[ERROR] Failed to create window surface." << endl;
        glfwTerminate();
        Cleanup();
        exit(7);
        return;
    }

    // Get a device.
    // fillModeNonSolid is required for the wireframe debug pipeline
    // (VK_POLYGON_MODE_LINE) — see CreateGraphicsPipeline. Virtually every
    // desktop/integrated GPU supports it; requiring it here just makes a
    // device that somehow doesn't fail selection with a clear error instead
    // of failing wireframe pipeline creation later with a validation error.
    VkPhysicalDeviceFeatures requiredFeatures{};
    requiredFeatures.fillModeNonSolid = VK_TRUE;

    vkb::PhysicalDeviceSelector physDeviceSelector(instance);
    auto physDeviceSelectorReturn = physDeviceSelector
        .set_surface(surface)
        .set_required_features(requiredFeatures)
        .select(); // TODO: Read settings.json if the user has a preferred device name.

    if (!physDeviceSelectorReturn)
    {
        cerr << "[ERROR] No suitable Vulkan devices: " << physDeviceSelectorReturn.error().message() << endl;
        Cleanup();
        exit(3);
        return;
    }
    physicalDevice = physDeviceSelectorReturn.value();
    cout << "[INFO] Found Vulkan GPU: " << physicalDevice.name << endl;
    cout << "[INFO] Vulkan API version: " << physicalDevice.properties.apiVersion << endl;

    // Get the logical device.
    vkb::DeviceBuilder deviceBuilder{physicalDevice};
    auto deviceBuilderReturn = deviceBuilder.build();

    if (!deviceBuilderReturn)
    {
        cerr << "[ERROR] Device creation failed: " << deviceBuilderReturn.error().message() << endl;
        Cleanup();
        exit(4);
        return;
    }
    device = deviceBuilderReturn.value();
    cout << "[INFO] Vulkan device initialized." << endl;

    // Set up the VMA allocator.
    VmaAllocatorCreateInfo allocInfo{};
    allocInfo.physicalDevice = physicalDevice.physical_device;
    allocInfo.device = device.device;
    allocInfo.instance = instance.instance;
    allocInfo.vulkanApiVersion = instance.api_version;
    vmaCreateAllocator(&allocInfo, &vmaAllocator);

    // Create the swapchain.
    vkb::SwapchainBuilder swapchainBuilder{device, surface};
    swapchainBuilder.set_desired_extent(WINDOW_WIDTH, WINDOW_HEIGHT);
    swapchainBuilder.set_desired_present_mode(PRESENT_MODE);
    swapchainBuilder.set_desired_min_image_count(BUFFER_SIZE);
    auto swapchainBuilderReturn = swapchainBuilder.build();

    if (!swapchainBuilderReturn)
    {
        cerr << "[ERROR] Failed to create swapchain: " << swapchainBuilderReturn.error().message() << endl;
        glfwTerminate();
        Cleanup();
        exit(8);
        return;
    }
    swapchain = swapchainBuilderReturn.value();

    // Get the queues from vkb::Device.
    graphicsQueue = device.get_queue(vkb::QueueType::graphics).value();
    presentQueue  = device.get_queue(vkb::QueueType::present).value();
    graphicsQueueFamilyIndex = device.get_queue_index(vkb::QueueType::graphics).value();
    cout << "[DEBUG] Graphics queue: " << graphicsQueue << endl;
    cout << "[DEBUG] Present queue: " << presentQueue << endl;

    // Set swapchainExtent.
    swapchainExtent = swapchain.extent;

    CreateDepthResources();
    CreateSwapchainImageViews();

    // Create the render pass.
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchain.image_format;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription attachments[] = {colorAttachment, depthAttachment};

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VkResult createRenderPassResult = vkCreateRenderPass(device.device, &renderPassInfo, nullptr, &renderPass);
    if (createRenderPassResult != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create render pass.");
    }

    // Create framebuffers.
    CreateFramebuffers();

    // Set up set layout bindings.
    VkDescriptorSetLayoutBinding cameraBinding{};
    cameraBinding.binding = 0;
    cameraBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    cameraBinding.descriptorCount = 1;
    cameraBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutBinding textureBinding{};
    textureBinding.binding = 0;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureBinding.descriptorCount = 1;
    textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &cameraBinding;
    vkCreateDescriptorSetLayout(GetDevice(), &layoutInfo, nullptr, &cameraSetLayout);

    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &textureBinding;
    vkCreateDescriptorSetLayout(GetDevice(), &layoutInfo, nullptr, &textureSetLayout);

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = 3;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }
    };
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 4; // 3 camera sets + 1 texture set
    vkCreateDescriptorPool(GetDevice(), &poolInfo, nullptr, &descriptorPool);

    for (int i = 0; i < 3; i++)
    {
        cameraUBOs[i] = CreateBuffer(sizeof(CameraUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        vmaMapMemory(vmaAllocator, cameraUBOs[i].allocation, &cameraUBOsMapped[i]); // stays mapped

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &cameraSetLayout;
        vkAllocateDescriptorSets(GetDevice(), &allocInfo, &cameraSets[i]);

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = cameraUBOs[i].buffer;
        bufferInfo.range = sizeof(CameraUBO);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = cameraSets[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(GetDevice(), 1, &write, 0, nullptr);
    }

    // Allocate texture descriptor set
    VkDescriptorSetAllocateInfo textureAllocInfo{};
    textureAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    textureAllocInfo.descriptorPool = descriptorPool;
    textureAllocInfo.descriptorSetCount = 1;
    textureAllocInfo.pSetLayouts = &textureSetLayout;
    vkAllocateDescriptorSets(GetDevice(), &textureAllocInfo, &textureSet);

    // Create the graphics pipeline.
    CreateGraphicsPipeline();

    // Non-cubic (transparent/cross/partial-shape) pass — needs
    // pipelineLayout above, since it reuses it.
    CreateNonCubicPipeline();

    // Create command pool and command buffers. Must happen before
    // EntityRenderer::Init() below: its UploadCubeMesh() uses
    // BeginOneShotCommands()/EndOneShotCommands(), which allocate from the
    // global `commandPool` — with no error checking on either the pool or
    // the command-buffer allocation, running it against a not-yet-created
    // (VK_NULL_HANDLE) pool crashed outright instead of throwing, which is
    // why it showed no error log at all before the process just exited.
    VkCommandPoolCreateInfo cmdPoolInfo{};
    cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.queueFamilyIndex = graphicsQueueFamilyIndex;
    cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // Allow re-recording

    if (vkCreateCommandPool(device.device, &cmdPoolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool!");
    }

    VkCommandBufferAllocateInfo bufferAllocInfo{};
    bufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    bufferAllocInfo.commandPool = commandPool;
    bufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    bufferAllocInfo.commandBufferCount = 3;

    if (vkAllocateCommandBuffers(device.device, &bufferAllocInfo, commandBuffers)) {
        throw std::runtime_error("Failed to allocate command buffers!");
    }

    // Entity render pass — needs the render pass and camera descriptor set
    // layout above, plus commandPool just above (see its own comment).
    EntityRenderer::Init();

    // Create semaphores and fences.
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < 3; i++)
    {
        if (vkCreateSemaphore(device.device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS)
        {
            throw runtime_error("[ERROR] Failed to create image available semaphores.");
        }

        if (vkCreateSemaphore(device.device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS)
        {
            throw runtime_error("[ERROR] Failed to create render finished semaphores.");
        }

        if (vkCreateFence(device.device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS)
        {
            throw runtime_error("[ERROR] Failed to create in flight fences.");
        }
    }
}

// Rebuilds everything sized off the swapchain extent, reusing the old
// swapchain (per Vulkan's recommended resize path) instead of tearing
// everything down and starting over. renderPass/pipeline/descriptor sets
// don't depend on the extent, so they're left alone. Only ever called from
// the render thread, between frames.
void RecreateSwapchain(int width, int height)
{
    if (width <= 0 || height <= 0) return; // Minimized — nothing to rebuild yet.

    vkDeviceWaitIdle(device.device);

    for (auto framebuffer : framebuffers) {
        vkDestroyFramebuffer(device.device, framebuffer, nullptr);
    }
    framebuffers.clear();

    for (auto view : imageViews) {
        vkDestroyImageView(device.device, view, nullptr);
    }
    imageViews.clear();

    if (depthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device.device, depthImageView, nullptr);
        depthImageView = VK_NULL_HANDLE;
    }
    if (depthImage != VK_NULL_HANDLE) {
        vmaDestroyImage(vmaAllocator, depthImage, depthImageAllocation);
        depthImage = VK_NULL_HANDLE;
    }

    vkb::Swapchain oldSwapchain = swapchain;

    vkb::SwapchainBuilder swapchainBuilder{device, surface};
    swapchainBuilder.set_desired_extent(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    swapchainBuilder.set_desired_present_mode(PRESENT_MODE);
    swapchainBuilder.set_desired_min_image_count(BUFFER_SIZE);
    swapchainBuilder.set_old_swapchain(oldSwapchain);
    auto swapchainBuilderReturn = swapchainBuilder.build();

    vkb::destroy_swapchain(oldSwapchain);

    if (!swapchainBuilderReturn)
    {
        throw std::runtime_error("[ERROR] Failed to recreate swapchain: " + swapchainBuilderReturn.error().message());
    }
    swapchain = swapchainBuilderReturn.value();
    swapchainExtent = swapchain.extent;

    CreateDepthResources();
    CreateSwapchainImageViews();
    CreateFramebuffers();
}

WindowMode GetWindowMode()
{
    return g_windowMode;
}

// F11 handler. Cycles Windowed <-> BorderlessFullscreen; ExclusiveFullscreen
// is deliberately skipped for now (see SetExclusiveFullscreen below) but the
// mode still exists so enabling it later doesn't need to reshape this enum
// or the cycle logic — just add it back into the switch.
void ToggleWindowMode()
{
    switch (g_windowMode)
    {
        case WindowMode::Windowed:
        {
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            if (!monitor) return;
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);

            // Remember the windowed geometry so we can restore it later.
            glfwGetWindowPos(window, &g_windowedX, &g_windowedY);
            glfwGetWindowSize(window, &g_windowedWidth, &g_windowedHeight);

            int monitorX, monitorY;
            glfwGetMonitorPos(monitor, &monitorX, &monitorY);

            // "Borderless fullscreen" here means a plain undecorated window
            // sized to cover the monitor, not a real (exclusive) mode
            // switch — this avoids the driver/OS display-mode change that
            // exclusive fullscreen would need.
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowMonitor(window, nullptr, monitorX, monitorY, mode->width, mode->height, 0);

            g_windowMode = WindowMode::BorderlessFullscreen;
            break;
        }
        case WindowMode::BorderlessFullscreen:
        case WindowMode::ExclusiveFullscreen:
        {
            glfwSetWindowMonitor(window, nullptr, g_windowedX, g_windowedY, g_windowedWidth, g_windowedHeight, 0);
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);

            g_windowMode = WindowMode::Windowed;
            break;
        }
    }
}

void Cleanup()
{
    if (device.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device.device);
    }

    // 1. Destroy manually created objects.
    for (int i = 0; i < 3; i++) {
        vkDestroySemaphore(device.device, imageAvailableSemaphores[i], nullptr);
        vkDestroySemaphore(device.device, renderFinishedSemaphores[i], nullptr);
        vkDestroyFence(device.device, inFlightFences[i], nullptr);
    }

    for (auto framebuffer : framebuffers) {
        vkDestroyFramebuffer(device.device, framebuffer, nullptr);
    }

    for (auto view : imageViews) {
        vkDestroyImageView(device.device, view, nullptr);
    }

    vkDestroyPipeline(device.device, graphicsPipeline, nullptr);
    vkDestroyPipeline(device.device, wireframePipeline, nullptr);
    vkDestroyPipeline(device.device, nonCubicPipeline, nullptr);
    vkDestroyPipelineLayout(device.device, pipelineLayout, nullptr);
    vkDestroyCommandPool(device.device, commandPool, nullptr);
    vkDestroyRenderPass(device.device, renderPass, nullptr);

    if (depthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device.device, depthImageView, nullptr);
    }
    if (depthImage != VK_NULL_HANDLE) {
        vmaDestroyImage(vmaAllocator, depthImage, depthImageAllocation);
    }

    // The camera UBOs (in allocatedBuffers, below) were left persistently
    // mapped by Init() — vmaDestroyBuffer asserts if its allocation still
    // has an outstanding map, so unmap them first.
    for (int i = 0; i < 3; i++) {
        if (cameraUBOsMapped[i] != nullptr) {
            vmaUnmapMemory(vmaAllocator, cameraUBOs[i].allocation);
            cameraUBOsMapped[i] = nullptr;
        }
    }
    for (auto& buf : allocatedBuffers)
    {
        vmaDestroyBuffer(vmaAllocator, buf.buffer, buf.allocation);
    }
    allocatedBuffers.clear();

    ChunkMesher::Shutdown();
    NonCubicMesher::Shutdown();
    EntityRenderer::Shutdown();

    if (vmaAllocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(vmaAllocator);
        vmaAllocator = VK_NULL_HANDLE;
    }

    // Destroy the bootstrap wrappers.
    if (swapchain.swapchain != VK_NULL_HANDLE) vkb::destroy_swapchain(swapchain);
    if (device.device != VK_NULL_HANDLE) vkb::destroy_device(device);
    if (surface != VK_NULL_HANDLE) vkb::destroy_surface(instance, surface);
    if (instance.instance != VK_NULL_HANDLE) vkb::destroy_instance(instance);

    if (window != nullptr) glfwDestroyWindow(window);
    glfwTerminate();
}

// Texture helper functions:

VkCommandBuffer BeginOneShotCommands()
{
    // commandPool must already exist (Init() creates it before anything
    // that calls this — see EntityRenderer::Init()'s own comment on why
    // that order matters) — a null pool here used to fail both calls below
    // silently and hand back/begin-record a garbage command buffer, which
    // crashed the process outright instead of throwing.
    if (commandPool == VK_NULL_HANDLE) {
        throw std::runtime_error("[ERROR] BeginOneShotCommands called before commandPool was created.");
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    if (vkAllocateCommandBuffers(GetDevice(), &allocInfo, &commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to allocate one-shot command buffer.");
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to begin one-shot command buffer.");
    }
    return commandBuffer;
}

void EndOneShotCommands(VkCommandBuffer cmd)
{
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to end one-shot command buffer.");
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to submit one-shot command buffer.");
    }
    vkQueueWaitIdle(graphicsQueue);

    vkFreeCommandBuffers(GetDevice(), commandPool, 1, &cmd);
}

void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, uint32_t baseMipLevel, uint32_t mipLevels, uint32_t baseArrayLayer, uint32_t layerCount, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = baseMipLevel;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = baseArrayLayer;
    barrier.subresourceRange.layerCount = layerCount;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        throw std::invalid_argument("unsupported layout transition!");
    }

    vkCmdPipelineBarrier(
        cmd,
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

// Setters:
