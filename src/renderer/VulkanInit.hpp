#pragma once
#ifndef VULKAN_INIT_H
#define VULKAN_INIT_H

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include "GlobalState.hpp"
#include "models/AllocatedBuffer.hpp"

using namespace Volcano;

// Setting placeholders:
constexpr const char* APP_NAME          = "Volcano Client";
constexpr const uint32_t APP_VERSION    = VK_MAKE_API_VERSION(1,0,1,1);
constexpr const char* ENGINE_NAME       = "Volcano Game Engine";
constexpr const uint32_t ENGINE_VERSION = VK_MAKE_API_VERSION(1,0,1,1);
constexpr const uint32_t WINDOW_WIDTH   = 854;
constexpr const uint32_t WINDOW_HEIGHT  = 480;
constexpr const auto PRESENT_MODE       = VK_PRESENT_MODE_FIFO_KHR; // VSync.
constexpr const uint8_t BUFFER_SIZE     = 2;
constexpr const uint16_t TARGET_FPS     = 60;
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
// (no backface cull, alpha blend, no depth write) differs from graphicsPipeline.
inline VkPipeline nonCubicPipeline;
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
inline std::vector<AllocatedBuffer> allocatedBuffers;
inline std::vector<AllocatedImage> allocatedImages;
inline VkDescriptorSetLayout textureSetLayout;
inline VkDescriptorSet textureSet;

// Depth Buffer:
inline VkImage depthImage;
inline VmaAllocation depthImageAllocation;
inline VkImageView depthImageView;
inline VkFormat depthFormat;

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
void CreateGraphicsPipeline();
// Must run after CreateGraphicsPipeline() — reuses its pipelineLayout.
void CreateNonCubicPipeline();
AllocatedBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool isUMA = false);
void Init(GlobalState* state);
void Cleanup();
VkRenderPass GetRenderPass();

// Resizing: rebuilds the swapchain (and everything sized off it — depth
// buffer, image views, framebuffers) in place, reusing the old swapchain
// per Vulkan's recommended resize path. Only safe to call from the render
// thread, between frames (never mid-recording).
void RecreateSwapchain(int width, int height);

// Window-mode toggling (F11). Exclusive fullscreen is intentionally left
// unimplemented/unselected for now — see ToggleWindowMode's definition.
WindowMode GetWindowMode();
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

#endif
