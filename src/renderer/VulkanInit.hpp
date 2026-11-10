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

// Engine/app identity
constexpr const char* APP_NAME          = "Volcano Client";
constexpr const uint32_t APP_VERSION    = VK_MAKE_API_VERSION(1,0,1,1);
constexpr const char* ENGINE_NAME       = "Volcano Game Engine";
constexpr const uint32_t ENGINE_VERSION = VK_MAKE_API_VERSION(1,0,1,1);

// Window.Width/Window.Height, Graphics.VSync and Graphics.BufferCount now
// live in GlobalState::config (see settings.json)

// Number of perpendicular billboard planes NonCubicMesher generates for
// cross-shaped blocks (grass, flowers, saplings, ...) - 2 matches vanilla
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
// Same attachments as renderPass but loading what's already there
inline VkRenderPass overlayRenderPass;
inline std::vector<VkFramebuffer> framebuffers;
inline std::vector<VkImageView> imageViews;
inline VkPipeline graphicsPipeline;
// Same pipeline, VK_POLYGON_MODE_LINE instead of FILL
inline VkPipeline wireframePipeline;
inline VkPipelineLayout pipelineLayout;
// Indirect-draw twins of the chunk pipelines above and below (same state and
// fragment shaders; the vertex shader reads each section's origin/fade from a
// per-instance attribute instead of push constants
inline VkPipeline graphicsIndirectPipeline = VK_NULL_HANDLE;
inline VkPipeline wireframeIndirectPipeline = VK_NULL_HANDLE;
inline VkPipeline nonCubicIndirectPipeline = VK_NULL_HANDLE;
inline VkPipeline translucentIndirectPipeline = VK_NULL_HANDLE;
// Both features enabled on the device (see Init).
inline bool indirectDrawSupported = false;
// Non-cubic (transparent/cross/partial-shape block) pass
inline VkPipeline nonCubicPipeline;
// Genuinely translucent pass (currently water only)
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
// Shadow receiving: set 2 (the shadow map as a comparison sampler) and the
// terrain pipelines that read it. The shadowed pipelines are VK_NULL_HANDLE
// until/unless their shaders build; the plain terrain pipelines never change.
inline VkDescriptorSetLayout shadowSetLayout;
inline VkDescriptorSet shadowSet;
inline VkPipelineLayout shadowedPipelineLayout = VK_NULL_HANDLE;
inline VkPipeline shadowedPipeline = VK_NULL_HANDLE;
inline VkPipeline shadowedIndirectPipeline = VK_NULL_HANDLE;
// Shadow-receiving twins of the non-cubic (cutout) and translucent pipelines,
// built from misc_shadow.* on the same shadowedPipelineLayout.
inline VkPipeline nonCubicShadowedPipeline = VK_NULL_HANDLE;
inline VkPipeline nonCubicShadowedIndirectPipeline = VK_NULL_HANDLE;
inline VkPipeline translucentShadowedPipeline = VK_NULL_HANDLE;
inline VkPipeline translucentShadowedIndirectPipeline = VK_NULL_HANDLE;
inline VkDescriptorSet textureSet;

// Depth Buffer:
inline VkImage depthImage;
inline VmaAllocation depthImageAllocation;
inline VkImageView depthImageView;
inline VkFormat depthFormat;

// Hi-Z occlusion pyramid
inline VkImage hizImage;
inline VmaAllocation hizImageAllocation;
inline std::vector<VkImageView> hizMipViews;
inline uint32_t hizMipLevels = 0;
inline VkExtent2D hizExtent{}; // mip 0's size

inline VkSampler hizDepthSampler; // For texelFetch-ing depthImage in hiz_copy.comp
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
inline VkRenderPass GetOverlayRenderPass() { return overlayRenderPass; }
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

// World pipelines
void CreateGraphicsPipeline(const Volcano::Engine::ShaderLibrary& shaders);
void DestroyGraphicsPipeline();
// Must run after CreateGraphicsPipeline() - reuses its pipelineLayout.
void CreateNonCubicPipeline(const Volcano::Engine::ShaderLibrary& shaders);
void DestroyNonCubicPipeline();
// Third pass, for geometry that's genuinely translucent rather than cutout
// (currently just water)
void CreateTranslucentPipeline(const Volcano::Engine::ShaderLibrary& shaders);
void DestroyTranslucentPipeline();

// Builds a VkShaderModule from shaders.Load(shaderName) ("terrain.vert",
// ...). Throws if the shader can't be found or the driver rejects it -
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

// Resizing: rebuilds the swapchain (and everything sized off it - depth
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
// least once
void NotifyFramePresented();

// Texture helper functions:
VkCommandBuffer BeginOneShotCommands();
void EndOneShotCommands(VkCommandBuffer cmd);
void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, uint32_t baseMipLevel, uint32_t mipLevels, uint32_t baseArrayLayer, uint32_t layerCount, VkImageLayout oldLayout, VkImageLayout newLayout);

// Hi-Z pyramid
void CreateHiZResources();
void DestroyHiZResources();
// Hi-Z pyramid
void CreateHiZPipelines(const Volcano::Engine::ShaderLibrary& shaders);
void DestroyHiZPipelines();

#endif
