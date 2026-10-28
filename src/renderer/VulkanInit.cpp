#include <cctype>
#include <chrono>
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
#include "gui/GUIController.hpp"
#include "../Helpers.hpp"
#include "models/Mesh.hpp"
#include "models/CameraUBO.hpp"
#include "models/PackedVertex.hpp"
#include "terrain/ChunkMesher.hpp"
#include "misc/MiscVertex.hpp"
#include "misc/NonCubicMesher.hpp"
#include "FullscreenExclusive.hpp"

using namespace std;

// Set once in Init(). Kept as a plain module-local pointer rather than the
// GLFW window user pointer, since InputHandler already claims that slot for
// its own key/cursor callbacks.
static GlobalState* g_globalState = nullptr;

// Current window mode and the windowed geometry to restore when leaving
// fullscreen (captured right before switching away from Windowed).
static WindowMode g_windowMode = WindowMode::Windowed;
static int g_windowedX = 0, g_windowedY = 0;
// Actual values are read from config and set once the window is created, in
// Init() - see the glfwGetWindowSize() call there.
static int g_windowedWidth = 0, g_windowedHeight = 0;

// Graphics.VSync/BufferCount from settings.json - read via g_globalState,
// which Init() sets before either of these can be called.
static VkPresentModeKHR DesiredPresentMode()
{
    bool vsync = std::get<bool>(g_globalState->config->Get("Graphics.VSync", true));
    return vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
}

static uint32_t DesiredBufferCount()
{
    return std::get<uint32_t>(g_globalState->config->Get("Graphics.BufferCount", uint32_t{2}));
}

// Settings every swapchain build shares. UNORM, not sRGB: textures are UNORM
// too and all lighting math is in gamma space, like vanilla.
static void ConfigureSwapchainBuilder(vkb::SwapchainBuilder& builder, uint32_t width, uint32_t height, bool exclusive)
{
    builder.set_desired_extent(width, height);
    builder.set_desired_format({ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR });
    builder.set_desired_present_mode(DesiredPresentMode());
    builder.set_desired_min_image_count(DesiredBufferCount());
    builder.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT); // F2 screenshots
    FullscreenExclusive::ConfigureSwapchain(builder, window, exclusive);
}

static void FramebufferSizeCallback(GLFWwindow* /*win*/, int width, int height)
{
    if (!g_globalState) return;
    g_globalState->pendingFramebufferWidth = width;
    g_globalState->pendingFramebufferHeight = height;
    g_globalState->framebufferResized = true;
}

#ifdef _WIN32
// Set once the render thread has presented at least one frame - see
// NotifyFramePresented(). GLFW_FOCUSED can already read true immediately
// after glfwCreateWindow() even though Windows hasn't actually shown/
// composited the window yet, so a focus-gained event (or an eager check at
// InputHandler construction - since removed) can fire before the window is
// really up. Locking the cursor at that point clips it to a window the
// desktop hasn't painted, which looks like a visible cursor trapped in a
// "ghost" window until the user clicks it. Withholding the lock until a
// frame has genuinely been presented fixes that ordering.
static bool g_firstFramePresented = false;

static void WindowFocusCallback(GLFWwindow* win, int focused)
{
    // Only fires inside glfwPollEvents(), which only the main thread calls
    // (VolcanoClient.cpp's message loop) - direct glfwSetInputMode calls
    // here are fine. Also mirrors focus into GlobalState so render-thread
    // code (NotifyFramePresented) can check it without calling GLFW itself.
    if (g_globalState) g_globalState->windowFocused.store(focused != 0);

    if (focused)
    {
        // Don't steal the cursor back from ImGui while a Screen (connect
        // screen, pause menu, etc.) is open - GUIController itself owns
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
    // fire to lock the cursor - so apply the same check here once, now
    // that the window is genuinely on screen. Runs on the render thread
    // (DrawFrame calls this), so it can't call glfwGetWindowAttrib/
    // glfwSetInputMode itself - see GlobalState::pendingCursorMode.
    if (g_globalState && g_globalState->windowFocused.load() && !GUIController::IsScreenOpen())
    {
        g_globalState->pendingCursorMode.store(GLFW_CURSOR_DISABLED);
    }
#endif
}

VkShaderModule CreateShaderModule(const Volcano::Engine::ShaderLibrary& shaders, const std::string& shaderName)
{
    return CreateShaderModule(shaders.Load(shaderName), shaderName);
}

VkShaderModule CreateShaderModule(const std::vector<char>& code, const std::string& debugName)
{
    if (code.empty())
    {
        throw std::runtime_error("[ERROR] Failed to load shader: " + debugName);
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(GetDevice(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create shader module: " + debugName);
    }
    return shaderModule;
}

// Builds the indirect-draw twin of a chunk pipeline: `base` with its vertex
// shader swapped for `indirectVert` and a second, per-instance vertex binding
// (location `instanceLocation`, one vec4 per section: xyz origin, w fade -
// see ChunkDrawBatch). Shares base's layout, so the push constant range is
// simply unused. Returns VK_NULL_HANDLE (and the caller keeps the per-section
// path) when indirect draw isn't available, or when a shader pack replaces
// `plainVert` - the pack's vertex shader has to keep running.
static VkPipeline CreateIndirectPipeline(const Volcano::Engine::ShaderLibrary& shaders,
    const char* plainVert, const char* indirectVert, const VkGraphicsPipelineCreateInfo& base,
    const VkVertexInputBindingDescription& vertexBinding,
    const VkVertexInputAttributeDescription* attributes, uint32_t attributeCount, uint32_t instanceLocation)
{
    if (!indirectDrawSupported) return VK_NULL_HANDLE;
    if (shaders.PackProvides(plainVert))
    {
        Volcano::Log::Info(std::string("Shader pack replaces ") + plainVert + " - indirect chunk drawing disabled for it.");
        return VK_NULL_HANDLE;
    }

    VkShaderModule vertModule = VK_NULL_HANDLE;
    try
    {
        vertModule = CreateShaderModule(shaders, indirectVert);
    }
    catch (const std::exception& e)
    {
        Volcano::Log::Error(std::string("[ERROR] ") + indirectVert + " failed to load (" + e.what() + ") - using per-section draws.");
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = { base.pStages[0], base.pStages[1] };
    stages[0].module = vertModule;

    VkVertexInputBindingDescription bindings[2] = {
        vertexBinding,
        { 1, sizeof(glm::vec4), VK_VERTEX_INPUT_RATE_INSTANCE }
    };
    std::vector<VkVertexInputAttributeDescription> attribs(attributes, attributes + attributeCount);
    attribs.push_back({ instanceLocation, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 });

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
    vertexInput.pVertexAttributeDescriptions = attribs.data();

    VkGraphicsPipelineCreateInfo info = base;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
    if (result != VK_SUCCESS)
    {
        Volcano::Log::Error(std::string("[ERROR] Indirect pipeline for ") + indirectVert + " failed - using per-section draws.");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

void CreateGraphicsPipeline(const Volcano::Engine::ShaderLibrary& shaders)
{
    VkDevice dev = GetDevice();

    // 1. LOAD COMPILED SPIR-V BINARIES (shader pack override, else built-in)
    VkShaderModule vertShaderModule = CreateShaderModule(shaders, "terrain.vert");
    VkShaderModule fragShaderModule = VK_NULL_HANDLE;
    try
    {
        fragShaderModule = CreateShaderModule(shaders, "terrain.frag");
    }
    catch (...)
    {
        vkDestroyShaderModule(dev, vertShaderModule, nullptr);
        throw;
    }

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
    // Backface culling is back ON: the asymmetry described below turned out
    // to be ChunkMesher's X axis using swapped u/v (for upright wall
    // textures), which flips that axis's winding handedness - fixed there
    // (see GreedyMeshAxis's handednessFlipped). All faces are now CCW from
    // outside in world space. If terrain renders inside-out, flip frontFace
    // below between COUNTER_CLOCKWISE and CLOCKWISE - it's uniform now.
    //
    // Historical note - previously: no backface culling for terrain. This
    // used to be VK_CULL_MODE_BACK_BIT
    // with VK_FRONT_FACE_CLOCKWISE, on the theory that ChunkMesher's greedy
    // quads are consistently wound CCW-from-outside in world space (see
    // GreedyMeshAxis's positiveCorners) and RenderThread's p[1][1] *= -1
    // projection flip (needed to land the image right-side-up) inverts that
    // to CW in framebuffer space uniformly for every face. In practice that
    // held for wall faces (X/Z-swept) but NOT for floor/ceiling faces
    // (Y-swept) - after correcting for the wall case, floors still rendered
    // inside-out (visible from below, culled from above, letting you see
    // through solid ground to bedrock) - a per-axis asymmetry the theory
    // above says shouldn't exist, and repeated attempts to hand-derive the
    // actual rule didn't resolve it. Rather than continue guessing, terrain
    // just draws both sides now: every face renders regardless of view
    // angle, so whichever winding is nearer the camera wins the depth test
    // and looks correct either way. This also means a player wedged inside
    // a solid block sees that block's own surface (like any other wall)
    // instead of every face flipping to show the far side of the world -
    // no more "wallhack" view from getting stuck. Costs roughly double the
    // fragment work versus perfectly-tuned culling; not worth chasing
    // further on an already-60fps-capped integrated GPU.
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
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
    pushRange.size = sizeof(glm::mat4) + sizeof(float);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    pipelineLayoutInfo.setLayoutCount = 2;
    VkDescriptorSetLayout setLayouts[] = { cameraSetLayout, textureSetLayout };
    pipelineLayoutInfo.pSetLayouts = setLayouts;

    // Shader modules are only needed until the pipelines below exist -
    // release them on every exit path, since a shader pack's replacement
    // failing pipeline creation is an expected (recoverable) outcome now.
    auto destroyModules = [&]()
    {
        vkDestroyShaderModule(dev, fragShaderModule, nullptr);
        vkDestroyShaderModule(dev, vertShaderModule, nullptr);
    };

    if (vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        destroyModules();
        throw std::runtime_error("[ERROR] Failed to create pipeline layout.");
    }

    cout << "Compiling shaders..." << endl;

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
        graphicsPipeline = VK_NULL_HANDLE;
        destroyModules();
        throw std::runtime_error("Failed to create graphics pipeline!");
    }

    // Wireframe variant of the same pipeline (F3+Z, see RenderThread) - a
    // debug tool for seeing where mesh geometry actually is versus what the
    // fill-mode result implies, e.g. telling "this face is missing
    // entirely" apart from "this face exists but lost the depth/winding
    // test" (which polygon fill alone can't distinguish). Shares every
    // other piece of state with the pipeline above - only polygonMode
    // differs - so it's built from the exact same CreateInfo, not a
    // parallel hand-copied one that could quietly drift out of sync.
    VkPipelineRasterizationStateCreateInfo wireframeRasterizer = rasterizer;
    wireframeRasterizer.polygonMode = VK_POLYGON_MODE_LINE;
    wireframeRasterizer.cullMode = VK_CULL_MODE_NONE;
    wireframeRasterizer.lineWidth = 1.0f; // >1.0 needs the wideLines feature, which isn't requested/guaranteed enabled.

    VkGraphicsPipelineCreateInfo wireframePipelineInfo = pipelineInfo;
    wireframePipelineInfo.pRasterizationState = &wireframeRasterizer;

    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &wireframePipelineInfo, nullptr, &wireframePipeline) != VK_SUCCESS) {
        wireframePipeline = VK_NULL_HANDLE;
        destroyModules();
        throw std::runtime_error("Failed to create wireframe graphics pipeline!");
    }

    // Indirect-draw twins (see CreateIndirectPipeline). A failure there just
    // leaves the handle null and the pass keeps the per-section path.
    graphicsIndirectPipeline = CreateIndirectPipeline(shaders, "terrain.vert", "terrain_indirect.vert", pipelineInfo,
        bindingDescription, attributeDescriptions.data(), static_cast<uint32_t>(attributeDescriptions.size()), 1);
    wireframeIndirectPipeline = CreateIndirectPipeline(shaders, "terrain.vert", "terrain_indirect.vert", wireframePipelineInfo,
        bindingDescription, attributeDescriptions.data(), static_cast<uint32_t>(attributeDescriptions.size()), 1);

    // Shadow-receiving twins: the same state with terrain_shadow.* (the
    // SHADOWS build of terrain.vert/.frag), one more descriptor set and its
    // own layout. Skipped when a shader pack replaces the terrain shaders (it
    // wouldn't receive shadows), and a failure only leaves them null.
    shadowedPipeline = VK_NULL_HANDLE;
    shadowedIndirectPipeline = VK_NULL_HANDLE;
    if (!shaders.PackProvides("terrain.vert") && !shaders.PackProvides("terrain.frag"))
    {
        VkShaderModule shadowVert = VK_NULL_HANDLE, shadowFrag = VK_NULL_HANDLE;
        try
        {
            shadowVert = CreateShaderModule(shaders, "terrain_shadow.vert");
            shadowFrag = CreateShaderModule(shaders, "terrain_shadow.frag");

            VkDescriptorSetLayout shadowedSetLayouts[] = { cameraSetLayout, textureSetLayout, shadowSetLayout };
            VkPipelineLayoutCreateInfo shadowedLayoutInfo = pipelineLayoutInfo;
            shadowedLayoutInfo.setLayoutCount = 3;
            shadowedLayoutInfo.pSetLayouts = shadowedSetLayouts;
            if (vkCreatePipelineLayout(dev, &shadowedLayoutInfo, nullptr, &shadowedPipelineLayout) != VK_SUCCESS)
            {
                throw std::runtime_error("shadowed pipeline layout");
            }

            VkPipelineShaderStageCreateInfo shadowedStages[2] = { shaderStages[0], shaderStages[1] };
            shadowedStages[0].module = shadowVert;
            shadowedStages[1].module = shadowFrag;
            VkGraphicsPipelineCreateInfo shadowedInfo = pipelineInfo;
            shadowedInfo.pStages = shadowedStages;
            shadowedInfo.layout = shadowedPipelineLayout;
            if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &shadowedInfo, nullptr, &shadowedPipeline) != VK_SUCCESS)
            {
                shadowedPipeline = VK_NULL_HANDLE;
                throw std::runtime_error("shadowed pipeline");
            }

            shadowedIndirectPipeline = CreateIndirectPipeline(shaders, "terrain.vert", "terrain_shadow_indirect.vert", shadowedInfo,
                bindingDescription, attributeDescriptions.data(), static_cast<uint32_t>(attributeDescriptions.size()), 1);
        }
        catch (const std::exception& e)
        {
            Volcano::Log::Error(std::string("[ERROR] Shadow-receiving terrain unavailable: ") + e.what());
            if (shadowedPipelineLayout != VK_NULL_HANDLE && shadowedPipeline == VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(dev, shadowedPipelineLayout, nullptr);
                shadowedPipelineLayout = VK_NULL_HANDLE;
            }
        }
        if (shadowFrag != VK_NULL_HANDLE) vkDestroyShaderModule(dev, shadowFrag, nullptr);
        if (shadowVert != VK_NULL_HANDLE) vkDestroyShaderModule(dev, shadowVert, nullptr);
    }

    // 11. CLEANUP TEMPORARY SHADER MODULES
    destroyModules();

    Volcano::Log::Info("Graphics pipeline successfully initialized.");
}

void DestroyGraphicsPipeline()
{
    VkDevice dev = GetDevice();
    if (graphicsPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, graphicsPipeline, nullptr);
    if (wireframePipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, wireframePipeline, nullptr);
    if (graphicsIndirectPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, graphicsIndirectPipeline, nullptr);
    if (wireframeIndirectPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, wireframeIndirectPipeline, nullptr);
    if (shadowedPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, shadowedPipeline, nullptr);
    if (shadowedIndirectPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, shadowedIndirectPipeline, nullptr);
    if (shadowedPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, shadowedPipelineLayout, nullptr);
    shadowedPipeline = VK_NULL_HANDLE;
    shadowedIndirectPipeline = VK_NULL_HANDLE;
    shadowedPipelineLayout = VK_NULL_HANDLE;
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
    graphicsPipeline = VK_NULL_HANDLE;
    wireframePipeline = VK_NULL_HANDLE;
    graphicsIndirectPipeline = VK_NULL_HANDLE;
    wireframeIndirectPipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
}

// Second pass for everything that isn't a fully opaque cube: transparent
// full cubes (glass, slime, ice, ...), partial-volume boxes (slabs,
// carpets, ...), and cross-shaped plants (grass, flowers, ...) - see
// NonCubicMesher. Reuses the terrain pass's pipelineLayout (identical push
// constant + descriptor set layout signature: camera UBO + texture array),
// so only a new VkPipeline with different fixed-function state is needed:
//  - No backface culling: this pass covers shapes meant to be seen from
//    both sides (a cross billboard, the inner face of a glass pane) rather
//    than closed solids, so the task's "shouldn't contribute to backface
//    culling" is satisfied by disabling culling for the pass entirely.
//  - Alpha blending on, and depth WRITE on alongside depth TEST.
//
// That depth write used to be off, on the reasoning that this is the
// "transparent" pass and one piece of it shouldn't occlude another. That
// reasoning doesn't survive contact with what this pass actually draws:
// stairs, slabs, carpets and snow layers are fully OPAQUE solids that just
// happen not to fill their voxel, and misc.frag discards every texel below
// 1% alpha, so this is really a cutout pass, not a translucent one. With
// writes off, nothing here could occlude anything else here - so a slab's
// own far-side and underside faces (drawn, since culling is off) painted
// straight over its near faces whenever they happened to be emitted later,
// which is what read as stairs and slabs being "inside-out", having
// "duplicate" surfaces against a neighbouring full cube, and flickering as
// the draw order shifted. Writing depth lets the depth test resolve all of
// that per-fragment, independent of emission order, without needing
// backface culling that the cross billboards still can't tolerate.
//
// The trade-off is genuinely translucent texels (0 < alpha < 1, e.g.
// stained glass) now write depth too and so can occlude translucent
// geometry behind them. Fixing that properly needs a third pass that draws
// truly translucent surfaces after this one with writes off - strictly
// better than every opaque partial shape in the world being drawn
// inside-out. That third pass now exists (CreateTranslucentPipeline, right
// below this function) for water, the first thing that actually needed it;
// stained glass still goes through this cutout pass unchanged.
// Shadow-receiving twins of one misc pipeline (cutout or translucent): `base`
// with misc_shadow.* and the shadowed layout terrain already built. Left null
// when that layout is missing, a shader pack replaces the misc shaders, or a
// shader fails to build.
static void CreateShadowedMiscPipelines(const Volcano::Engine::ShaderLibrary& shaders, const VkGraphicsPipelineCreateInfo& base,
    const VkVertexInputBindingDescription& binding, const VkVertexInputAttributeDescription* attributes,
    uint32_t attributeCount, VkPipeline& direct, VkPipeline& indirect)
{
    direct = VK_NULL_HANDLE;
    indirect = VK_NULL_HANDLE;
    if (shadowedPipelineLayout == VK_NULL_HANDLE) return;
    if (shaders.PackProvides("misc.vert") || shaders.PackProvides("misc.frag")) return;

    VkDevice dev = GetDevice();
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    try
    {
        vert = CreateShaderModule(shaders, "misc_shadow.vert");
        frag = CreateShaderModule(shaders, "misc_shadow.frag");

        VkPipelineShaderStageCreateInfo stages[2] = { base.pStages[0], base.pStages[1] };
        stages[0].module = vert;
        stages[1].module = frag;
        VkGraphicsPipelineCreateInfo info = base;
        info.pStages = stages;
        info.layout = shadowedPipelineLayout;
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &direct) != VK_SUCCESS)
        {
            direct = VK_NULL_HANDLE;
            throw std::runtime_error("shadowed misc pipeline");
        }
        indirect = CreateIndirectPipeline(shaders, "misc.vert", "misc_shadow_indirect.vert", info,
            binding, attributes, attributeCount, 3);
    }
    catch (const std::exception& e)
    {
        Volcano::Log::Error(std::string("[ERROR] Shadow-receiving non-cubic blocks unavailable: ") + e.what());
    }
    if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(dev, frag, nullptr);
    if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(dev, vert, nullptr);
}

void CreateNonCubicPipeline(const Volcano::Engine::ShaderLibrary& shaders)
{
    VkDevice dev = GetDevice();

    VkShaderModule vertShaderModule = CreateShaderModule(shaders, "misc.vert");
    VkShaderModule fragShaderModule = VK_NULL_HANDLE;
    try
    {
        fragShaderModule = CreateShaderModule(shaders, "misc.frag");
    }
    catch (...)
    {
        vkDestroyShaderModule(dev, vertShaderModule, nullptr);
        throw;
    }

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
    depthStencil.depthWriteEnable = VK_TRUE; // See this function's own comment on why.
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
    pipelineInfo.layout = pipelineLayout; // Shared with the opaque terrain pipeline - see this function's own comment.
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    VkResult pipelineResult = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &nonCubicPipeline);
    nonCubicIndirectPipeline = CreateIndirectPipeline(shaders, "misc.vert", "misc_indirect.vert", pipelineInfo,
        bindingDescription, attributeDescriptions.data(), static_cast<uint32_t>(attributeDescriptions.size()), 3);
    CreateShadowedMiscPipelines(shaders, pipelineInfo, bindingDescription, attributeDescriptions.data(),
        static_cast<uint32_t>(attributeDescriptions.size()), nonCubicShadowedPipeline, nonCubicShadowedIndirectPipeline);

    vkDestroyShaderModule(dev, fragShaderModule, nullptr);
    vkDestroyShaderModule(dev, vertShaderModule, nullptr);

    if (pipelineResult != VK_SUCCESS) {
        nonCubicPipeline = VK_NULL_HANDLE;
        throw std::runtime_error("Failed to create non-cubic graphics pipeline!");
    }

    Volcano::Log::Info("Non-cubic graphics pipeline successfully initialized.");
}

// Third pass, for geometry that's genuinely translucent rather than cutout -
// currently just water (NonCubicMesher's Liquid shape). Identical to
// CreateNonCubicPipeline in every way (same shaders, same MiscVertex layout,
// same pipelineLayout, same alpha blend factors, no backface culling) except
// depth WRITE is off here: two overlapping translucent surfaces (looking
// through one water surface at another, or at stained glass once that also
// moves to this pass) need to actually blend, not have the nearer one win
// the depth test outright the way the cutout pass's writes-on approach needs
// for stairs/slabs - see CreateNonCubicPipeline's own comment for why THAT
// pass writes depth. Depth TEST stays on, so this still draws correctly
// behind opaque terrain/entities/cutout geometry; NonCubicPass::Record draws
// this pass right after the cutout pass (and sorts its meshes back-to-front
// by chunk) so translucency reads right at least at chunk granularity - see
// its own comment for the per-chunk-only caveat that leaves.
void CreateTranslucentPipeline(const Volcano::Engine::ShaderLibrary& shaders)
{
    VkDevice dev = GetDevice();

    VkShaderModule vertShaderModule = CreateShaderModule(shaders, "misc.vert");
    VkShaderModule fragShaderModule = VK_NULL_HANDLE;
    try
    {
        fragShaderModule = CreateShaderModule(shaders, "misc.frag");
    }
    catch (...)
    {
        vkDestroyShaderModule(dev, vertShaderModule, nullptr);
        throw;
    }

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
    rasterizer.cullMode = VK_CULL_MODE_NONE; // Same reasoning as CreateNonCubicPipeline's own comment.
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE; // The one difference from nonCubicPipeline - see this function's own comment.
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
    pipelineInfo.layout = pipelineLayout; // Shared with the opaque terrain pipeline - see CreateNonCubicPipeline's own comment.
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    VkResult translucentPipelineResult = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &translucentPipeline);
    translucentIndirectPipeline = CreateIndirectPipeline(shaders, "misc.vert", "misc_indirect.vert", pipelineInfo,
        bindingDescription, attributeDescriptions.data(), static_cast<uint32_t>(attributeDescriptions.size()), 3);
    CreateShadowedMiscPipelines(shaders, pipelineInfo, bindingDescription, attributeDescriptions.data(),
        static_cast<uint32_t>(attributeDescriptions.size()), translucentShadowedPipeline, translucentShadowedIndirectPipeline);

    vkDestroyShaderModule(dev, fragShaderModule, nullptr);
    vkDestroyShaderModule(dev, vertShaderModule, nullptr);

    if (translucentPipelineResult != VK_SUCCESS) {
        translucentPipeline = VK_NULL_HANDLE;
        throw std::runtime_error("Failed to create translucent graphics pipeline!");
    }

    Volcano::Log::Info("Translucent graphics pipeline successfully initialized.");
}

void DestroyNonCubicPipeline()
{
    if (nonCubicPipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), nonCubicPipeline, nullptr);
    if (nonCubicIndirectPipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), nonCubicIndirectPipeline, nullptr);
    if (nonCubicShadowedPipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), nonCubicShadowedPipeline, nullptr);
    if (nonCubicShadowedIndirectPipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), nonCubicShadowedIndirectPipeline, nullptr);
    nonCubicShadowedPipeline = VK_NULL_HANDLE;
    nonCubicShadowedIndirectPipeline = VK_NULL_HANDLE;
    nonCubicPipeline = VK_NULL_HANDLE;
    nonCubicIndirectPipeline = VK_NULL_HANDLE;
}

void DestroyTranslucentPipeline()
{
    if (translucentPipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), translucentPipeline, nullptr);
    if (translucentIndirectPipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), translucentIndirectPipeline, nullptr);
    if (translucentShadowedPipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), translucentShadowedPipeline, nullptr);
    if (translucentShadowedIndirectPipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), translucentShadowedIndirectPipeline, nullptr);
    translucentShadowedPipeline = VK_NULL_HANDLE;
    translucentShadowedIndirectPipeline = VK_NULL_HANDLE;
    translucentPipeline = VK_NULL_HANDLE;
    translucentIndirectPipeline = VK_NULL_HANDLE;
}

// Create a buffer with VMA. The caller owns it and must vmaDestroyBuffer it.
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
    // SAMPLED, on top of the attachment bit every frame's render pass needs,
    // so HiZBuildPass can texelFetch it after the render pass ends - see
    // CreateHiZResources' own comment.
    depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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

// Resolution-dependent Hi-Z resources: the pyramid image itself, one
// VK_IMAGE_USAGE_STORAGE_BIT view per mip (imageLoad/imageStore need a
// specific mip, not the whole chain), the descriptor set layouts the
// compute shaders bind against, and the actual descriptor sets (allocated
// and pointed at those views here, since neither depends on which pipeline
// eventually uses them - mirrors textureSetLayout/textureSet being created
// here while graphicsPipeline itself is TerrainPass-owned). See
// VulkanInit.hpp's own comment on the pyramid's mip 0 / max-reduction
// convention.
void CreateHiZResources()
{
    hizExtent = swapchainExtent;
    // floor(log2(max dimension)) + 1 - the classic full mip chain count,
    // capped so an unusually large window doesn't walk all the way down to
    // 1x1 (nothing meaningful to cull against at that point, and it's more
    // mips than any of this frame's chunk AABBs would ever need to query).
    uint32_t maxDim = std::max(hizExtent.width, hizExtent.height);
    hizMipLevels = 1;
    while ((maxDim >> hizMipLevels) > 0 && hizMipLevels < 10) hizMipLevels++;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { hizExtent.width, hizExtent.height, 1 };
    imageInfo.mipLevels = hizMipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R32_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    if (vmaCreateImage(vmaAllocator, &imageInfo, &allocInfo, &hizImage, &hizImageAllocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create Hi-Z pyramid image!");
    }

    // One-shot UNDEFINED -> GENERAL for every mip, maintained as a
    // permanent invariant from here on - HiZBuildPass::Record only ever
    // issues memory barriers against this image, never layout transitions,
    // since GENERAL already supports both imageLoad/imageStore and being a
    // vkCmdCopyImageToBuffer source (the diagnostic readback).
    {
        VkCommandBuffer oneShot = BeginOneShotCommands();
        VkImageMemoryBarrier toGeneral{};
        toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = hizImage;
        toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, hizMipLevels, 0, 1 };
        toGeneral.srcAccessMask = 0;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(oneShot, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toGeneral);
        EndOneShotCommands(oneShot);
    }

    hizMipViews.resize(hizMipLevels);
    for (uint32_t i = 0; i < hizMipLevels; i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = hizImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32_SFLOAT;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1 };
        if (vkCreateImageView(device.device, &viewInfo, nullptr, &hizMipViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("[ERROR] Failed to create Hi-Z mip image view!");
        }
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device.device, &samplerInfo, nullptr, &hizDepthSampler) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create Hi-Z depth sampler!");
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device.device, &layoutInfo, nullptr, &hizCopySetLayout) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create Hi-Z copy descriptor set layout!");
    }

    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; // src mip, instead of the depth sampler.
    if (vkCreateDescriptorSetLayout(device.device, &layoutInfo, nullptr, &hizDownsampleSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create Hi-Z downsample descriptor set layout!");
    }

    // hizDownsampleSets[i] reads mip i, writes mip i+1 - one set per
    // mip-to-mip transition, plus one more for the depth-to-mip-0 copy.
    uint32_t downsampleCount = hizMipLevels > 0 ? hizMipLevels - 1 : 0;
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    poolSizes[1] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 + downsampleCount * 2 };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1 + downsampleCount;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device.device, &poolInfo, nullptr, &hizDescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create Hi-Z descriptor pool!");
    }

    VkDescriptorSetAllocateInfo copyAllocInfo{};
    copyAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    copyAllocInfo.descriptorPool = hizDescriptorPool;
    copyAllocInfo.descriptorSetCount = 1;
    copyAllocInfo.pSetLayouts = &hizCopySetLayout;
    if (vkAllocateDescriptorSets(device.device, &copyAllocInfo, &hizCopySet) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to allocate Hi-Z copy descriptor set!");
    }

    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler = hizDepthSampler;
    depthInfo.imageView = depthImageView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo mip0Info{};
    mip0Info.imageView = hizMipViews[0];
    mip0Info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet copyWrites[2]{};
    copyWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    copyWrites[0].dstSet = hizCopySet;
    copyWrites[0].dstBinding = 0;
    copyWrites[0].descriptorCount = 1;
    copyWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    copyWrites[0].pImageInfo = &depthInfo;
    copyWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    copyWrites[1].dstSet = hizCopySet;
    copyWrites[1].dstBinding = 1;
    copyWrites[1].descriptorCount = 1;
    copyWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    copyWrites[1].pImageInfo = &mip0Info;
    vkUpdateDescriptorSets(device.device, 2, copyWrites, 0, nullptr);

    hizDownsampleSets.resize(downsampleCount);
    std::vector<VkDescriptorSetLayout> downsampleLayouts(downsampleCount, hizDownsampleSetLayout);
    if (downsampleCount > 0) {
        VkDescriptorSetAllocateInfo downsampleAllocInfo{};
        downsampleAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        downsampleAllocInfo.descriptorPool = hizDescriptorPool;
        downsampleAllocInfo.descriptorSetCount = downsampleCount;
        downsampleAllocInfo.pSetLayouts = downsampleLayouts.data();
        if (vkAllocateDescriptorSets(device.device, &downsampleAllocInfo, hizDownsampleSets.data()) != VK_SUCCESS) {
            throw std::runtime_error("[ERROR] Failed to allocate Hi-Z downsample descriptor sets!");
        }

        for (uint32_t i = 0; i < downsampleCount; i++) {
            VkDescriptorImageInfo srcInfo{};
            srcInfo.imageView = hizMipViews[i];
            srcInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo dstInfo{};
            dstInfo.imageView = hizMipViews[i + 1];
            dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet writes[2]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = hizDownsampleSets[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[0].pImageInfo = &srcInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = hizDownsampleSets[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].pImageInfo = &dstInfo;
            vkUpdateDescriptorSets(device.device, 2, writes, 0, nullptr);
        }
    }

    // The CPU readback buffers live in HiZBuildPass (per frame slot), not
    // here - they're a fixed size regardless of resolution.
}

void DestroyHiZResources()
{
    if (hizDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device.device, hizDescriptorPool, nullptr);
        hizDescriptorPool = VK_NULL_HANDLE;
    }
    hizCopySet = VK_NULL_HANDLE;
    hizDownsampleSets.clear();

    if (hizCopySetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device.device, hizCopySetLayout, nullptr);
        hizCopySetLayout = VK_NULL_HANDLE;
    }
    if (hizDownsampleSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device.device, hizDownsampleSetLayout, nullptr);
        hizDownsampleSetLayout = VK_NULL_HANDLE;
    }
    if (hizDepthSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device.device, hizDepthSampler, nullptr);
        hizDepthSampler = VK_NULL_HANDLE;
    }

    for (VkImageView view : hizMipViews) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device.device, view, nullptr);
    }
    hizMipViews.clear();

    if (hizImage != VK_NULL_HANDLE) {
        vmaDestroyImage(vmaAllocator, hizImage, hizImageAllocation);
        hizImage = VK_NULL_HANDLE;
    }
    hizMipLevels = 0;
}

// Shader-dependent Hi-Z compute pipelines - see CreateHiZResources' own
// comment on the split between this and that. Neither shader takes push
// constants: both derive their working size from imageSize() on the bound
// images directly, so there's nothing per-dispatch to push.
void CreateHiZPipelines(const Volcano::Engine::ShaderLibrary& shaders)
{
    VkDevice dev = GetDevice();

    VkPipelineLayoutCreateInfo copyLayoutInfo{};
    copyLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    copyLayoutInfo.setLayoutCount = 1;
    copyLayoutInfo.pSetLayouts = &hizCopySetLayout;
    if (vkCreatePipelineLayout(dev, &copyLayoutInfo, nullptr, &hizCopyPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create Hi-Z copy pipeline layout!");
    }

    VkPipelineLayoutCreateInfo downsampleLayoutInfo{};
    downsampleLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    downsampleLayoutInfo.setLayoutCount = 1;
    downsampleLayoutInfo.pSetLayouts = &hizDownsampleSetLayout;
    if (vkCreatePipelineLayout(dev, &downsampleLayoutInfo, nullptr, &hizDownsamplePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create Hi-Z downsample pipeline layout!");
    }

    VkShaderModule copyModule = CreateShaderModule(shaders, "hiz_copy.comp");
    VkPipelineShaderStageCreateInfo copyStage{};
    copyStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    copyStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    copyStage.module = copyModule;
    copyStage.pName = "main";

    VkComputePipelineCreateInfo copyPipelineInfo{};
    copyPipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    copyPipelineInfo.stage = copyStage;
    copyPipelineInfo.layout = hizCopyPipelineLayout;
    VkResult copyResult = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &copyPipelineInfo, nullptr, &hizCopyPipeline);
    vkDestroyShaderModule(dev, copyModule, nullptr);
    if (copyResult != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create Hi-Z copy pipeline!");
    }

    VkShaderModule downsampleModule = CreateShaderModule(shaders, "hiz_downsample.comp");
    VkPipelineShaderStageCreateInfo downsampleStage{};
    downsampleStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    downsampleStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    downsampleStage.module = downsampleModule;
    downsampleStage.pName = "main";

    VkComputePipelineCreateInfo downsamplePipelineInfo{};
    downsamplePipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    downsamplePipelineInfo.stage = downsampleStage;
    downsamplePipelineInfo.layout = hizDownsamplePipelineLayout;
    VkResult downsampleResult = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &downsamplePipelineInfo, nullptr, &hizDownsamplePipeline);
    vkDestroyShaderModule(dev, downsampleModule, nullptr);
    if (downsampleResult != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create Hi-Z downsample pipeline!");
    }
}

void DestroyHiZPipelines()
{
    VkDevice dev = GetDevice();
    if (hizCopyPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(dev, hizCopyPipeline, nullptr); hizCopyPipeline = VK_NULL_HANDLE; }
    if (hizDownsamplePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(dev, hizDownsamplePipeline, nullptr); hizDownsamplePipeline = VK_NULL_HANDLE; }
    if (hizCopyPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(dev, hizCopyPipelineLayout, nullptr); hizCopyPipelineLayout = VK_NULL_HANDLE; }
    if (hizDownsamplePipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(dev, hizDownsamplePipelineLayout, nullptr); hizDownsamplePipelineLayout = VK_NULL_HANDLE; }
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

    // config must be ready before the window is created - the very next
    // line reads Window.Width/Height from it.
    g_globalState = state;

    uint32_t windowWidth = std::get<uint32_t>(state->config->Get("Window.Width", uint32_t{854}));
    uint32_t windowHeight = std::get<uint32_t>(state->config->Get("Window.Height", uint32_t{480}));

    // Create the window.
    window = glfwCreateWindow(static_cast<int>(windowWidth), static_cast<int>(windowHeight), APP_NAME, nullptr, nullptr);

    if (!window)
    {
        cerr << "[ERROR] Failed to create window." << endl;
        glfwTerminate();
        Cleanup();
        exit(6);
        return;
    }
    state->window = window;
    glfwGetWindowPos(window, &g_windowedX, &g_windowedY);
    glfwGetWindowSize(window, &g_windowedWidth, &g_windowedHeight);

    // Framebuffer-size changes (user resize, and F11's glfwSetWindowMonitor
    // calls) land here on the main thread; the render thread picks the flag
    // up between frames and recreates the swapchain there, since that's the
    // only thread allowed to touch it.
    glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);

    // Window.Mode is NOT applied here: switching the display mode this early
    // (before the splash screen) meant the focus churn around startup could
    // auto-iconify the fullscreen window - GLFW then restores the desktop
    // mode and leaves the window at the old fullscreen size in the corner.
    // RenderThread's first PollInputs applies it instead (see
    // ApplyWindowSettings - g_appliedConfigVersion starts unset).

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

    // Make an instance. Passing our own vkGetInstanceProcAddr (statically
    // imported through Vulkan::Vulkan/vulkan-1.lib, an ordinary PE import
    // table entry) makes vk-bootstrap skip its own internal
    // LoadLibrary("vulkan-1.dll") + a cascade of GetProcAddress-by-name
    // calls to bootstrap itself - harmless and universal for any Vulkan
    // app, but that exact runtime "load a system DLL, then walk its
    // exports by string name" pattern is also what a reflective DLL
    // loader/injector does, and it's what got this unsigned, no-reputation
    // binary flagged as Trojan.Injector/Rootkit by Malwarebytes and Avast.
    // Keep the instance's required API version at this project's existing
    // 1.1 floor rather than whatever vkEnumerateInstanceVersion reports.
    // That call returns the Vulkan LOADER's own compiled ceiling, which is
    // not a guarantee that every installed ICD (GPU driver) actually
    // implements it - requesting it via require_api_version() made
    // vk-bootstrap's PhysicalDeviceSelector reject any physical device
    // whose real VkPhysicalDeviceProperties::apiVersion fell short,
    // producing "no_suitable_device" on this machine. The debug overlay's
    // reported Vulkan version doesn't come from this requested value
    // anyway - see physicalDevice.properties.apiVersion below, which is
    // the selected device's own true supported version, independent of
    // what we request here.
    vkb::InstanceBuilder instanceBuilder(vkGetInstanceProcAddr);
    FullscreenExclusive::ConfigureInstance(instanceBuilder);
    auto instanceReturn = instanceBuilder
        .set_app_name(APP_NAME)
        .set_app_version(APP_VERSION)
        .set_engine_name(ENGINE_NAME)
        .set_engine_version(ENGINE_VERSION)
        .require_api_version(1, 1, 0)
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
    // (VK_POLYGON_MODE_LINE) - see CreateGraphicsPipeline. Virtually every
    // desktop/integrated GPU supports it; requiring it here just makes a
    // device that somehow doesn't fail selection with a clear error instead
    // of failing wireframe pipeline creation later with a validation error.
    VkPhysicalDeviceFeatures requiredFeatures{};
    requiredFeatures.fillModeNonSolid = VK_TRUE;

    vkb::PhysicalDeviceSelector physDeviceSelector(instance);
    physDeviceSelector
        .set_surface(surface)
        .set_required_features(requiredFeatures);

    // Graphics.PreferredDevice (settings.json): when non-empty, pick the
    // device whose name contains it (case-insensitively, so "nvidia" is
    // enough for "NVIDIA GeForce RTX 4070") instead of whatever
    // vk-bootstrap's own ranking puts first - the usual reason to set it
    // being a laptop where the default pick is the integrated GPU. Empty
    // (the default this Get() writes on first run) keeps that ranking.
    //
    // A name matching nothing is a warning, not a failure: set_name() would
    // make selection fail outright, which turns a stale settings.json into
    // a client that won't start. Falling through to the default device and
    // logging the names that *were* available makes the setting
    // self-correcting instead.
    std::string preferredDevice = std::get<std::string>(
        g_globalState->config->Get("Graphics.PreferredDevice", std::string{}));
    if (!preferredDevice.empty())
    {
        auto lowered = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::string needle = lowered(preferredDevice);

        // select_devices() applies the same surface/feature requirements as
        // select() below, so anything it returns is already a device this
        // client can actually run on - the name match only reorders them.
        auto candidates = physDeviceSelector.select_devices();
        if (candidates)
        {
            std::string available;
            bool matched = false;
            for (const vkb::PhysicalDevice& candidate : candidates.value())
            {
                if (!available.empty()) available += ", ";
                available += candidate.name;
                if (!matched && lowered(candidate.name).find(needle) != std::string::npos)
                {
                    physDeviceSelector.set_name(candidate.name);
                    cout << "Graphics.PreferredDevice \"" << preferredDevice
                         << "\" matched Vulkan device: " << candidate.name << endl;
                    matched = true;
                }
            }
            if (!matched)
            {
                cerr << "[WARN] Graphics.PreferredDevice \"" << preferredDevice
                     << "\" matched no suitable Vulkan device (available: " << available
                     << ") - falling back to the default device." << endl;
            }
        }
        else
        {
            cerr << "[WARN] Could not enumerate Vulkan devices to honor Graphics.PreferredDevice: "
                 << candidates.error().message() << " - falling back to the default device." << endl;
        }
    }

    auto physDeviceSelectorReturn = physDeviceSelector.select();

    if (!physDeviceSelectorReturn)
    {
        cerr << "[ERROR] No suitable Vulkan devices: " << physDeviceSelectorReturn.error().message() << endl;
        Cleanup();
        exit(3);
        return;
    }
    physicalDevice = physDeviceSelectorReturn.value();
    FullscreenExclusive::ConfigureDevice(physicalDevice);
    cout << "Found Vulkan GPU: " << physicalDevice.name << endl;
    cout << "Vulkan API version: " << physicalDevice.properties.apiVersion << endl;
    g_globalState->systemInfo.DetectGPU(physicalDevice.properties.deviceName,
        physicalDevice.properties.vendorID, physicalDevice.properties.apiVersion);

    // Get the logical device.
    vkb::DeviceBuilder deviceBuilder{physicalDevice};
    // Chunk sections draw with one multi-draw indirect call per list when
    // both features exist (Graphics.IndirectDraw); otherwise the per-section
    // path is used, so these are optional.
    VkPhysicalDeviceFeatures indirectFeatures{};
    indirectFeatures.multiDrawIndirect = VK_TRUE;
    indirectFeatures.drawIndirectFirstInstance = VK_TRUE;
    indirectDrawSupported = physicalDevice.enable_features_if_present(indirectFeatures);
    cout << "Multi-draw indirect: " << (indirectDrawSupported ? "supported" : "not supported") << endl;

    auto deviceBuilderReturn = deviceBuilder.build();

    if (!deviceBuilderReturn)
    {
        cerr << "[ERROR] Device creation failed: " << deviceBuilderReturn.error().message() << endl;
        Cleanup();
        exit(4);
        return;
    }
    device = deviceBuilderReturn.value();
    cout << "Vulkan device initialized." << endl;

    // Set up the VMA allocator.
    VmaAllocatorCreateInfo allocInfo{};
    allocInfo.physicalDevice = physicalDevice.physical_device;
    allocInfo.device = device.device;
    allocInfo.instance = instance.instance;
    allocInfo.vulkanApiVersion = instance.api_version;
    vmaCreateAllocator(&allocInfo, &vmaAllocator);

    // Create the swapchain.
    vkb::SwapchainBuilder swapchainBuilder{device, surface};
    ConfigureSwapchainBuilder(swapchainBuilder, windowWidth, windowHeight, g_windowMode == WindowMode::ExclusiveFullscreen);
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
    FullscreenExclusive::Acquire(device.device, swapchain.swapchain);
    // Records what vk-bootstrap actually granted, not just what
    // DesiredPresentMode() asked for - see GlobalState::presentMode's own
    // comment for why those can differ.
    g_globalState->presentMode = static_cast<uint8_t>(swapchain.present_mode);
    Log::Info(std::string("[GFX] Present mode " + std::to_string(static_cast<int>(swapchain.present_mode)) + " (requested " + std::to_string(static_cast<int>(DesiredPresentMode())) + "; 0 = immediate, 1 = mailbox, 2 = FIFO)"));

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
    // STORE, not DONT_CARE: HiZBuildPass samples this after the render pass
    // ends to build the Hi-Z pyramid - DONT_CARE let the driver discard it
    // (read back as a constant 0).
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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

    // The overlay pass: the same attachments, but keeping the finished frame
    // (and its depth) instead of clearing it. Compatible with renderPass, so
    // the same framebuffers and pipelines work with both.
    {
        VkAttachmentDescription overlayAttachments[] = { colorAttachment, depthAttachment };
        overlayAttachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        overlayAttachments[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        overlayAttachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        overlayAttachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDependency overlayDependency{};
        overlayDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        overlayDependency.dstSubpass = 0;
        overlayDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        overlayDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        overlayDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        overlayDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo overlayInfo = renderPassInfo;
        overlayInfo.pAttachments = overlayAttachments;
        overlayInfo.pDependencies = &overlayDependency;
        if (vkCreateRenderPass(device.device, &overlayInfo, nullptr, &overlayRenderPass) != VK_SUCCESS)
        {
            throw std::runtime_error("[ERROR] Failed to create overlay render pass.");
        }
    }

    // Create framebuffers.
    CreateFramebuffers();

    // Set up set layout bindings.
    VkDescriptorSetLayoutBinding cameraBinding{};
    cameraBinding.binding = 0;
    cameraBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    cameraBinding.descriptorCount = 1;
    // Fragment too: terrain.frag/misc.frag read lighting and fog from it.
    cameraBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

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

    VkDescriptorSetLayoutBinding shadowBinding{};
    shadowBinding.binding = 0;
    shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBinding.descriptorCount = 1;
    shadowBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo shadowLayoutInfo{};
    shadowLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    shadowLayoutInfo.bindingCount = 1;
    shadowLayoutInfo.pBindings = &shadowBinding;
    vkCreateDescriptorSetLayout(GetDevice(), &shadowLayoutInfo, nullptr, &shadowSetLayout);

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = 3;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 }
    };
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 5; // 3 camera sets + 1 texture set + 1 shadow set
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

    // The shadow set is written by VulkanRenderEngine::SetShadowMap once a map exists.
    VkDescriptorSetAllocateInfo shadowAllocInfo = textureAllocInfo;
    shadowAllocInfo.pSetLayouts = &shadowSetLayout;
    vkAllocateDescriptorSets(GetDevice(), &shadowAllocInfo, &shadowSet);

    // Pipelines (terrain, non-cubic, translucent, entities, selection, ...)
    // are no longer built here - each belongs to a RenderPass the engine
    // creates right after this returns (see VulkanRenderEngine::Init), so a
    // shader reload can rebuild them without touching the device/swapchain
    // set up here.

    // Create command pool and command buffers. Must happen before
    // EntityRenderer::Init() (EntityPass::Create): its UploadCubeMesh() uses
    // BeginOneShotCommands()/EndOneShotCommands(), which allocate from the
    // global `commandPool` - with no error checking on either the pool or
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

    // Must come after commandPool exists - CreateHiZResources() uses
    // BeginOneShotCommands()/EndOneShotCommands() for its one-shot layout
    // transition, which allocate from `commandPool` (see the comment on
    // command pool creation above for what running this against a
    // not-yet-created pool actually does: crashes outright, no error log).
    // RecreateSwapchain() calls this same function right after
    // CreateDepthResources() instead - commandPool already exists there,
    // since Init() has always run once before any resize.
    CreateHiZResources();

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
    if (width <= 0 || height <= 0) return; // Minimized - nothing to rebuild yet.

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
    DestroyHiZResources(); // Recreated below at the new resolution, alongside CreateDepthResources().

    vkb::Swapchain oldSwapchain = swapchain;
    FullscreenExclusive::Release(device.device, oldSwapchain.swapchain);

    auto buildSwapchain = [&](bool exclusive, const vkb::Swapchain* previous)
    {
        vkb::SwapchainBuilder swapchainBuilder{device, surface};
        ConfigureSwapchainBuilder(swapchainBuilder, static_cast<uint32_t>(width), static_cast<uint32_t>(height), exclusive);
        if (previous) swapchainBuilder.set_old_swapchain(*previous);
        return swapchainBuilder.build();
    };

    auto swapchainBuilderReturn = buildSwapchain(g_windowMode == WindowMode::ExclusiveFullscreen, &oldSwapchain);
    // A failed create still retires the old swapchain, so it can't be reused.
    vkb::destroy_swapchain(oldSwapchain);

    if (!swapchainBuilderReturn)
    {
        Log::Error("[ERROR] Swapchain recreation failed (" + swapchainBuilderReturn.error().message()
            + ", VkResult " + std::to_string(static_cast<int>(swapchainBuilderReturn.vk_result()))
            + ") - retrying without exclusive fullscreen.");
        swapchainBuilderReturn = buildSwapchain(false, nullptr);
    }
    if (!swapchainBuilderReturn)
    {
        throw std::runtime_error("Failed to recreate the swapchain: " + swapchainBuilderReturn.error().message()
            + " (VkResult " + std::to_string(static_cast<int>(swapchainBuilderReturn.vk_result())) + ")");
    }
    swapchain = swapchainBuilderReturn.value();
    FullscreenExclusive::Acquire(device.device, swapchain.swapchain);
    swapchainExtent = swapchain.extent;
    // See the Init() call site's own comment on this same line.
    g_globalState->presentMode = static_cast<uint8_t>(swapchain.present_mode);
    Log::Info(std::string("[GFX] Present mode " + std::to_string(static_cast<int>(swapchain.present_mode)) + " (requested " + std::to_string(static_cast<int>(DesiredPresentMode())) + "; 0 = immediate, 1 = mailbox, 2 = FIFO)"));

    CreateDepthResources();
    CreateHiZResources();
    CreateSwapchainImageViews();
    CreateFramebuffers();
}

WindowMode GetWindowMode()
{
    return g_windowMode;
}

// Settings ApplyWindowSettings last applied - compared against config so
// only real changes touch the window (a settings write to some unrelated
// key shouldn't re-set the window size or re-enter fullscreen).
struct AppliedWindowSettings {
    WindowMode mode = WindowMode::Windowed;
    uint32_t width = 0, height = 0;                        // Window.Width/Height (windowed size)
    uint32_t fsWidth = 0, fsHeight = 0, fsRefreshRate = 0; // Window.FullscreenMode.*
    bool vsync = true;                                     // Graphics.VSync
    uint32_t bufferCount = 0;                              // Graphics.BufferCount
};
static AppliedWindowSettings g_appliedWindow;
static uint64_t g_appliedConfigVersion = UINT64_MAX;

// Moves the window into `target`, using the given exclusive-fullscreen video
// mode (0 = the monitor's current width/height/refresh rate).
static void SetWindowMode(WindowMode target, uint32_t fsWidth, uint32_t fsHeight, uint32_t fsRefreshRate)
{
    // Remember windowed geometry before leaving Windowed, to restore later.
    if (g_windowMode == WindowMode::Windowed && target != WindowMode::Windowed)
    {
        glfwGetWindowPos(window, &g_windowedX, &g_windowedY);
        glfwGetWindowSize(window, &g_windowedWidth, &g_windowedHeight);
    }

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* current = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (target != WindowMode::Windowed && (!monitor || !current)) return;

    switch (target)
    {
        case WindowMode::Windowed:
            glfwSetWindowMonitor(window, nullptr, g_windowedX, g_windowedY, g_windowedWidth, g_windowedHeight, 0);
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
            break;

        case WindowMode::BorderlessFullscreen:
        {
            // A plain undecorated window covering the monitor at its current
            // mode - no display-mode change, so alt-tab is instant.
            int monitorX, monitorY;
            glfwGetMonitorPos(monitor, &monitorX, &monitorY);
            glfwSetWindowMonitor(window, nullptr, monitorX, monitorY, current->width, current->height, 0);
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
            break;
        }

        case WindowMode::ExclusiveFullscreen:
        {
            // Real fullscreen at Window.FullscreenMode.*. GLFW would silently
            // pick the "closest" mode to anything unsupported (a swapped
            // 768x1366 landed on a 4:3 mode), so require an exact match from
            // the monitor's own mode list and otherwise fall back to the
            // desktop mode, which never needs a display-mode change.
            int width = fsWidth > 0 ? static_cast<int>(fsWidth) : current->width;
            int height = fsHeight > 0 ? static_cast<int>(fsHeight) : current->height;
            int refresh = fsRefreshRate > 0 ? static_cast<int>(fsRefreshRate) : current->refreshRate;

            int modeCount = 0;
            const GLFWvidmode* modes = glfwGetVideoModes(monitor, &modeCount);
            bool supported = false;
            for (int i = 0; i < modeCount; i++)
            {
                if (modes[i].width == width && modes[i].height == height && modes[i].refreshRate == refresh)
                {
                    supported = true;
                    break;
                }
            }
            if (!supported)
            {
                Log::Error("[WINDOW] Fullscreen mode " + std::to_string(width) + "x" + std::to_string(height)
                    + "@" + std::to_string(refresh) + " isn't supported by this monitor"
                    + (height > width ? " (width/height swapped?)" : "") + " - using the desktop mode "
                    + std::to_string(current->width) + "x" + std::to_string(current->height)
                    + "@" + std::to_string(current->refreshRate) + " instead.");
                width = current->width;
                height = current->height;
                refresh = current->refreshRate;
            }

            glfwSetWindowMonitor(window, monitor, 0, 0, width, height, refresh);
            break;
        }
    }

    g_windowMode = target;

    // Force a swapchain rebuild even if the size didn't change, so the
    // exclusive-fullscreen request (see FullscreenExclusive) follows the mode.
    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    FramebufferSizeCallback(window, framebufferWidth, framebufferHeight);
}

// Reads Window.Mode (0 windowed, 1 borderless, 2 fullscreen), Window.Width/
// Height and Window.FullscreenMode.Width/Height/RefreshRate, and applies
// whatever changed since last time. Cheap when nothing changed (one atomic
// compare), so RenderThread calls it every frame - that's what lets a
// Settings screen change any of these live. Main thread only (GLFW).
void ApplyWindowSettings()
{
    Config* config = g_globalState ? g_globalState->config : nullptr;
    if (!config || !window) return;
    uint64_t configVersion = config->GetVersion();
    if (configVersion == g_appliedConfigVersion) return;
    g_appliedConfigVersion = configVersion;

    uint32_t modeValue = std::get<uint32_t>(config->Get("Window.Mode", uint32_t{0}));
    AppliedWindowSettings wanted;
    wanted.mode = modeValue == 1 ? WindowMode::BorderlessFullscreen
                : modeValue == 2 ? WindowMode::ExclusiveFullscreen
                : WindowMode::Windowed;
    wanted.width = std::get<uint32_t>(config->Get("Window.Width", uint32_t{854}));
    wanted.height = std::get<uint32_t>(config->Get("Window.Height", uint32_t{480}));
    wanted.fsWidth = std::get<uint32_t>(config->Get("Window.FullscreenMode.Width", uint32_t{0}));
    wanted.fsHeight = std::get<uint32_t>(config->Get("Window.FullscreenMode.Height", uint32_t{0}));
    wanted.fsRefreshRate = std::get<uint32_t>(config->Get("Window.FullscreenMode.RefreshRate", uint32_t{0}));
    wanted.vsync = std::get<bool>(config->Get("Graphics.VSync", true));
    wanted.bufferCount = DesiredBufferCount();

    // Present mode and image count are baked into the swapchain: rebuild it.
    bool firstApply = g_appliedWindow.bufferCount == 0;
    if (!firstApply && (wanted.vsync != g_appliedWindow.vsync || wanted.bufferCount != g_appliedWindow.bufferCount))
    {
        int framebufferWidth, framebufferHeight;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        FramebufferSizeCallback(window, framebufferWidth, framebufferHeight);
    }

    // A changed windowed size applies right away if windowed, otherwise it's
    // just what Windowed restores to later.
    bool windowedSizeChanged = wanted.width != g_appliedWindow.width || wanted.height != g_appliedWindow.height;
    if (windowedSizeChanged && wanted.width > 0 && wanted.height > 0)
    {
        g_windowedWidth = static_cast<int>(wanted.width);
        g_windowedHeight = static_cast<int>(wanted.height);
        if (g_windowMode == WindowMode::Windowed && wanted.mode == WindowMode::Windowed)
        {
            glfwSetWindowSize(window, g_windowedWidth, g_windowedHeight);
        }
    }

    bool fullscreenModeChanged = wanted.fsWidth != g_appliedWindow.fsWidth
        || wanted.fsHeight != g_appliedWindow.fsHeight
        || wanted.fsRefreshRate != g_appliedWindow.fsRefreshRate;
    if (wanted.mode != g_windowMode
        || (wanted.mode == WindowMode::ExclusiveFullscreen && fullscreenModeChanged))
    {
        SetWindowMode(wanted.mode, wanted.fsWidth, wanted.fsHeight, wanted.fsRefreshRate);
    }

    g_appliedWindow = wanted;
}

// F11 handler: Windowed -> borderless, anything else -> Windowed. Goes
// through Window.Mode (persisted) so the Settings screen and settings.json
// always agree with what's on screen; ApplyWindowSettings does the switch.
void ToggleWindowMode()
{
    Config* config = g_globalState ? g_globalState->config : nullptr;
    if (!config) return;
    uint32_t next = g_windowMode == WindowMode::Windowed ? 1u : 0u;
    config->Set("Window.Mode", next);
    ApplyWindowSettings();
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

    // Pass-owned pipelines are normally gone already (RenderEngine::
    // StopRendering destroys every pass) - these are idempotent, and only
    // matter if startup failed before the passes could be torn down.
    DestroyNonCubicPipeline();
    DestroyTranslucentPipeline();
    DestroyGraphicsPipeline();
    vkDestroyCommandPool(device.device, commandPool, nullptr);
    vkDestroyRenderPass(device.device, overlayRenderPass, nullptr);
    vkDestroyRenderPass(device.device, renderPass, nullptr);

    if (depthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device.device, depthImageView, nullptr);
    }
    if (depthImage != VK_NULL_HANDLE) {
        vmaDestroyImage(vmaAllocator, depthImage, depthImageAllocation);
    }

    // HiZBuildPass::Destroy() normally already ran this (RenderEngine's
    // shutdown path), same "idempotent, only matters if startup failed
    // early" reasoning as the other pass-owned pipeline destroys above.
    DestroyHiZPipelines();
    DestroyHiZResources();

    // Camera UBOs stay mapped from Init(); unmap before destroying.
    for (int i = 0; i < 3; i++)
    {
        if (cameraUBOsMapped[i] != nullptr)
        {
            vmaUnmapMemory(vmaAllocator, cameraUBOs[i].allocation);
            cameraUBOsMapped[i] = nullptr;
        }
        if (cameraUBOs[i].buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(vmaAllocator, cameraUBOs[i].buffer, cameraUBOs[i].allocation);
            cameraUBOs[i] = AllocatedBuffer{};
        }
    }

    // EntityRenderer/SelectionRenderer are shut down by their passes
    // (EntityPass/SelectionPass::Destroy), not here.
    ChunkMesher::Shutdown();
    NonCubicMesher::Shutdown();

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
    // that calls this - see EntityRenderer::Init()'s own comment on why
    // that order matters) - a null pool here used to fail both calls below
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
