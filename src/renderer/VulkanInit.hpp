#pragma once
#ifndef VULKAN_INIT_H
#define VULKAN_INIT_H

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <VkBootstrap.h>

// Setting placeholders:
constexpr const char* APP_NAME          = "Volcano Client";
constexpr const uint32_t APP_VERSION    = VK_MAKE_API_VERSION(1,0,1,1);
constexpr const char* ENGINE_NAME       = "Volcano Game Engine";
constexpr const uint32_t ENGINE_VERSION = VK_MAKE_API_VERSION(1,0,1,1);
constexpr const uint32_t WINDOW_WIDTH   = 854;
constexpr const uint32_t WINDOW_HEIGHT  = 480;
constexpr const auto PRESENT_MODE       = VK_PRESENT_MODE_MAILBOX_KHR;
constexpr const uint8_t BUFFER_SIZE     = 3;
constexpr const uint16_t TARGET_FPS     = 60;

// Global handles:
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
inline VkPipelineLayout pipelineLayout;
inline VkQueue graphicsQueue;
inline VkQueue presentQueue;
inline uint32_t graphicsQueueFamilyIndex;
inline VkCommandPool commandPool;
inline VkCommandBuffer commandBuffers[3];
inline VkSemaphore imageAvailableSemaphores[3];
inline VkSemaphore renderFinishedSemaphores[3];
inline VkFence inFlightFences[3];

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

// Functions:
void CreateGraphicsPipeline();
void Init();
void Cleanup();

#endif