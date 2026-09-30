#pragma once
#ifndef VOLCANO_FULLSCREEN_EXCLUSIVE_H
#define VOLCANO_FULLSCREEN_EXCLUSIVE_H

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>

struct GLFWwindow;

namespace Volcano {

// Optional VK_EXT_full_screen_exclusive support for Window.Mode 2. Queried,
// never required: without it, mode 2 is still a GLFW fullscreen window and
// the driver decides whether it really goes exclusive (Intel keeps DWM).
// Lives in its own file to keep <windows.h> out of VulkanInit.cpp.
class FullscreenExclusive
{
public:
    // Before instance creation: enables the instance-side dependency if present.
    static void ConfigureInstance(vkb::InstanceBuilder& builder);
    // After device selection, before device creation.
    static void ConfigureDevice(vkb::PhysicalDevice& physicalDevice);
    // Before each swapchain build. `exclusive` = the window is in mode 2.
    static void ConfigureSwapchain(vkb::SwapchainBuilder& builder, GLFWwindow* window, bool exclusive);
    // After each swapchain build: takes exclusive control if it was requested.
    static void Acquire(VkDevice device, VkSwapchainKHR swapchain);
    // Before recreating/destroying a swapchain that may hold exclusive mode.
    static void Release(VkDevice device, VkSwapchainKHR swapchain);

    static bool IsSupported() { return supported; }

private:
    static inline bool instanceSupported = false;
    static inline bool supported = false;
    static inline bool requested = false; // last ConfigureSwapchain asked for exclusive
    static inline bool acquired = false;
};

} // namespace Volcano

#endif
