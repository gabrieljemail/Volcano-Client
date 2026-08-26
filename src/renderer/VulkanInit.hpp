#pragma once
#ifndef VULKAN_INIT_H
#define VULKAN_INIT_H

#include <cstdint>
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
vkb::Instance instance;
vkb::PhysicalDevice physicalDevice;
vkb::Device device;
VkSurfaceKHR surface;
vkb::Swapchain swapchain;
GLFWwindow* window;
VkRenderPass renderPass;
vector<VkFramebuffer> framebuffers;
vector<VkImageView> imageViews;
VkPipeline graphicsPipeline;
uint32_t graphicsQueueFamilyIndex;
VkCommandPool commandPool;
VkCommandBuffer commandBuffers;
VkSemaphore imageAvailableSemaphores[3];
VkSemaphore renderFinishedSemaphores[3];
VkFence inFlightFences[3];

// Getters:
VkInstance GetInstance() { return instance; }
VkPhysicalDevice GetPhysicalDevice() { return physicalDevice; }
VkDevice GetDevice() { return device; }
VkSurfaceKHR GetSurface() { return surface; }
VkSwapchainKHR GetSwapchain() { return swapchain; }
VkRenderPass GetRenderPass() { return renderPass; }
vector<VkFramebuffer> GetSwapchainFramebuffers() { return framebuffers; }
vector<VkImageView> GetSwapchainImageViews() { return imageViews; }
VkPipeline GetGraphicsPipeline() { return graphicsPipeline; }
uint32_t GetGraphicsQueueFamilyIndex() { return graphicsQueueFamilyIndex; }
VkCommandPool GetCommandPool() { return commandPool; }
VkCommandBuffer GetCommandBuffers() { return commandBuffers; }
void GetImageAvailableSemaphores(VkSemaphore*& outVar) { outVar = imageAvailableSemaphores; }
void GetRenderFinishedSemaphores(VkSemaphore*& outVar) { outVar = renderFinishedSemaphores; }
void GetInFlightFences(VkFence*& outVar) { outVar = inFlightFences; }

// Functions:
void CreateGraphicsPipeline();
void Init();

#endif