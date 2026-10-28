#pragma once
#ifndef VULKAN_INIT_H
#define VULKAN_INIT_H

#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include "GlobalState.hpp"
#include "models/AllocatedBuffer.hpp"
#include "engine/ShaderLibrary.hpp"

using namespace Volcano;

// Engine/app identity — not a user setting, so still a plain constant.
constexpr const char* APP_NAME          = "Volcano Client";
constexpr const uint32_t APP_VERSION    = VK_MAKE_API_VERSION(1,0,1,1);
constexpr const char* ENGINE_NAME       = "Volcano Game Engine";
constexpr const uint32_t ENGINE_VERSION = VK_MAKE_API_VERSION(1,0,1,1);

// Window.Width/Window.Height, Graphics.VSync and Graphics.BufferCount now
// live in GlobalState::config (see settings.json) — read via
// DesiredWindowSize()/DesiredPresentMode()/DesiredBufferCount() in
// VulkanInit.cpp rather than as compile-time constants.

// Number of perpendicular billboard planes NonCubicMesher generates for
// cross-shaped blocks (grass, flowers, saplings, ...) — 2 matches vanilla
// Minecraft's default X-shape.
constexpr const uint8_t CROSS_BILLBOARD_COUNT = 2;

// Global handles:
struct GLFWwindow;
inline vkb::Instance instance;
inline vkb::PhysicalDevice physicalDevice;
inline vkb::Device device;
inline VkSurfaceKHR surface;
inline vkb::Swapchain swapchain;
inline VkExtent2D swapchainExtent;
inline GLFWwindow* window;
inline VkRenderPass renderPass;
inline std::vector<VkFramebuffer> framebuffers;
inline std::vector<VkImageView> imageViews;
inline VkPipeline graphicsPipeline;
// Same pipeline, VK_POLYGON_MODE_LINE instead of FILL — see
// CreateGraphicsPipeline. Toggled by F3, see RenderThread.
inline VkPipeline wireframePipeline;
inline VkPipelineLayout pipelineLayout;
// Non-cubic (transparent/cross/partial-shape block) pass — see
// CreateNonCubicPipeline(). Shares pipelineLayout (same push constant +
// descriptor set layout signature) since only the fixed-function state
// (no backface cull, alpha blend, depth write ON — see that function's own
// comment on why) differs from graphicsPipeline.
inline VkPipeline nonCubicPipeline;
// Genuinely translucent pass (currently water only) — see
// CreateTranslucentPipeline(). Same shaders/vertex format/blend state as
// nonCubicPipeline; the only fixed-function difference is depth write OFF,
// which is exactly the "third pass" nonCubicPipeline's own comment describes
// as worth adding once something genuinely translucent needed it.
inline VkPipeline translucentPipeline;
inline VkQueue graphicsQueue;
inline VkQueue presentQueue;
inline uint32_t graphicsQueueFamilyIndex;
inline VkCommandPool commandPool;
inline VkCommandBuffer commandBuffers[3];
inline VkSemaphore imageAvailableSemaphores[3];
inline VkSemaphore renderFinishedSemaphores[3];
inline VkFence inFlightFences[3];
inline VmaAllocator vmaAllocator;
inline VkDescriptorSetLayout cameraSetLayout;
inline VkDescriptorPool descriptorPool;
inline VkDescriptorSet cameraSets[3];
inline AllocatedBuffer cameraUBOs[3];
inline void* cameraUBOsMapped[3];
inline std::vector<AllocatedImage> allocatedImages;
inline VkDescriptorSetLayout textureSetLayout;
inline VkDescriptorSet textureSet;

// Depth Buffer:
inline VkImage depthImage;
inline VmaAllocation depthImageAllocation;
inline VkImageView depthImageView;
inline VkFormat depthFormat;

// Hi-Z occlusion pyramid — see HiZBuildPass's own comment for the full
// picture. mip 0 is a same-resolution copy of depthImage; each mip above it
// is the MAX (farthest, since depth here is non-reversed) of the 2x2 texels
// below it. hizMipViews[i] is mip i alone (VK_IMAGE_USAGE_STORAGE_BIT, for
// imageLoad/imageStore) — resized alongside depthImage in RecreateSwapchain.
inline VkImage hizImage;
inline VmaAllocation hizImageAllocation;
inline std::vector<VkImageView> hizMipViews;
inline uint32_t hizMipLevels = 0;
inline VkExtent2D hizExtent{}; // mip 0's size — always swapchainExtent.

inline VkSampler hizDepthSampler; // For texelFetch-ing depthImage in hiz_copy.comp — no filtering happens, so its filter/wrap settings don't matter.
inline VkDescriptorSetLayout hizCopySetLayout;       // binding 0: depthImage (sampler2D), binding 1: mip 0 (storage image, write).
inline VkDescriptorSetLayout hizDownsampleSetLayout; // binding 0: src mip (storage image, read), binding 1: dst mip (storage image, write).
inline VkPipelineLayout hizCopyPipelineLayout;
inline VkPipelineLayout hizDownsamplePipelineLayout;
inline VkPipeline hizCopyPipeline;
inline VkPipeline hizDownsamplePipeline;
inline VkDescriptorPool hizDescriptorPool;
inline VkDescriptorSet hizCopySet;
inline std::vector<VkDescriptorSet> hizDownsampleSets; // size hizMipLevels - 1; set[i] reads mip i, writes mip i+1.

// Getters:
inline VkInstance GetInstance() { return instance.instance; }
inline VkPhysicalDevice GetPhysicalDevice() { return physicalDevice.physical_device; }
inline VkDevice GetDevice() { return device.device; }
inline VkSurfaceKHR GetSurface() { return surface; }
inline VkSwapchainKHR GetSwapchain() { return swapchain.swapchain; }
inline VkRenderPass GetRenderPass() { return renderPass; }
// inline vector<VkFramebuffer> GetSwapchainFramebuffers() { return framebuffers; }
// inline vector<VkImageView> GetSwapchainImageViews() { return imageViews; }
inline VkPipeline GetGraphicsPipeline() { return graphicsPipeline; }
inline VkPipelineLayout GetPipelineLayout() { return pipelineLayout; }
inline uint32_t GetGraphicsQueueFamilyIndex() { return graphicsQueueFamilyIndex; }
inline VkCommandPool GetCommandPool() { return commandPool; }
inline void GetCommandBuffers(VkCommandBuffer*& outVar) { outVar = commandBuffers; }
inline void GetImageAvailableSemaphores(VkSemaphore*& outVar) { outVar = imageAvailableSemaphores; }
inline void GetRenderFinishedSemaphores(VkSemaphore*& outVar) { outVar = renderFinishedSemaphores; }
inline void GetInFlightFences(VkFence*& outVar) { outVar = inFlightFences; }
inline VkDescriptorSet GetTextureSet() { return textureSet; }

// Functions:

// World pipelines — terrain (plus its wireframe twin) and non-cubic. Owned
// by the engine's TerrainPass/NonCubicPass (see engine/passes/), which
// call these from their Create()/Destroy(), and again on a shader reload;
// both Destroy functions are idempotent. Shaders come from the
// ShaderLibrary, so a shader pack can replace terrain.*/misc.*.
void CreateGraphicsPipeline(const Volcano::Engine::ShaderLibrary& shaders);
void DestroyGraphicsPipeline();
// Must run after CreateGraphicsPipeline() — reuses its pipelineLayout.
void CreateNonCubicPipeline(const Volcano::Engine::ShaderLibrary& shaders);
void DestroyNonCubicPipeline();
// Third pass, for geometry that's genuinely translucent rather than cutout
// (currently just water) — see CreateTranslucentPipeline's own comment.
// Must run after CreateGraphicsPipeline() — reuses its pipelineLayout, same as CreateNonCubicPipeline().
void CreateTranslucentPipeline(const Volcano::Engine::ShaderLibrary& shaders);
void DestroyTranslucentPipeline();

// Builds a VkShaderModule from shaders.Load(shaderName) ("terrain.vert",
// ...). Throws if the shader can't be found or the driver rejects it —
// which is what lets RenderEngine fall back to the built-in shaders when a
// shader pack's replacement is broken.
VkShaderModule CreateShaderModule(const Volcano::Engine::ShaderLibrary& shaders, const std::string& shaderName);
// Same, from SPIR-V already in hand (e.g. ShaderLibrary::LoadFromPack()).
// debugName only appears in the exception message.
VkShaderModule CreateShaderModule(const std::vector<char>& code, const std::string& debugName);

AllocatedBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool isUMA = false);
void Init(GlobalState* state);
void Cleanup();
VkRenderPass GetRenderPass();

// Resizing: rebuilds the swapchain (and everything sized off it — depth
// buffer, image views, framebuffers) in place, reusing the old swapchain
// per Vulkan's recommended resize path. Only safe to call from the render
// thread, between frames (never mid-recording).
void RecreateSwapchain(int width, int height);

// Window modes, driven by Window.Mode (0 windowed, 1 borderless, 2
// exclusive fullscreen at Window.FullscreenMode.Width/Height/RefreshRate).
// ApplyWindowSettings re-applies them whenever the config changes (e.g. a
// Settings screen); ToggleWindowMode is F11. Both main thread only.
WindowMode GetWindowMode();
void ApplyWindowSettings();
void ToggleWindowMode();

// Called by the render thread right after its first successful present.
// On Windows, the cursor-lock-on-focus machinery (WindowFocusCallback)
// deliberately withholds locking the cursor until this has happened at
// least once — see the definition for why.
void NotifyFramePresented();

// Texture helper functions:
VkCommandBuffer BeginOneShotCommands();
void EndOneShotCommands(VkCommandBuffer cmd);
void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, uint32_t baseMipLevel, uint32_t mipLevels, uint32_t baseArrayLayer, uint32_t layerCount, VkImageLayout oldLayout, VkImageLayout newLayout);

// Hi-Z pyramid — resolution-dependent resources (image, mip views, sampler,
// descriptor set layouts + allocated/updated sets, readback buffer). Called
// from Init()/RecreateSwapchain() alongside CreateDepthResources(), same
// lifecycle. Destroy is idempotent, same convention as the other Destroy*
// functions here.
void CreateHiZResources();
void DestroyHiZResources();
// Hi-Z pyramid — shader-dependent pipelines only (the descriptor set layouts
// they use are created by CreateHiZResources() above). Called from
// HiZBuildPass::Create()/Destroy(), not from Init() directly — mirrors
// CreateGraphicsPipeline()'s own split between VulkanInit-owned base
// resources and pass-owned pipelines.
void CreateHiZPipelines(const Volcano::Engine::ShaderLibrary& shaders);
void DestroyHiZPipelines();

#endif
