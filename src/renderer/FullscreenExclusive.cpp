#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <GLFW/glfw3native.h>
#endif

#include "FullscreenExclusive.hpp"
#include "Logger.hpp"
#include <string>

namespace Volcano {

void FullscreenExclusive::ConfigureInstance(vkb::InstanceBuilder& builder)
{
#ifdef _WIN32
    auto systemInfo = vkb::SystemInfo::get_system_info();
    instanceSupported = systemInfo && systemInfo->is_extension_available(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    if (instanceSupported)
    {
        builder.enable_extension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    }
#else
    (void)builder;
#endif
}

void FullscreenExclusive::ConfigureDevice(vkb::PhysicalDevice& physicalDevice)
{
#ifdef _WIN32
    supported = instanceSupported && physicalDevice.enable_extension_if_present(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
#else
    (void)physicalDevice;
#endif
    Log::Info(std::string("[WINDOW] VK_EXT_full_screen_exclusive: ") + (supported ? "available" : "not available"));
}

void FullscreenExclusive::ConfigureSwapchain(vkb::SwapchainBuilder& builder, GLFWwindow* window, bool exclusive)
{
    requested = false;
#ifdef _WIN32
    if (!supported || !exclusive) return;

    // Static: vk-bootstrap keeps the pointers until build().
    static VkSurfaceFullScreenExclusiveInfoEXT exclusiveInfo{};
    static VkSurfaceFullScreenExclusiveWin32InfoEXT win32Info{};

    win32Info.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT;
    win32Info.hmonitor = MonitorFromWindow(glfwGetWin32Window(window), MONITOR_DEFAULTTOPRIMARY);

    exclusiveInfo.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
    exclusiveInfo.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;

    builder.add_pNext(&exclusiveInfo);
    builder.add_pNext(&win32Info);
    requested = true;
#else
    (void)builder;
    (void)window;
    (void)exclusive;
#endif
}

void FullscreenExclusive::Acquire(VkDevice device, VkSwapchainKHR swapchain)
{
#ifdef _WIN32
    if (!requested) return;

    auto acquire = reinterpret_cast<PFN_vkAcquireFullScreenExclusiveModeEXT>(
        vkGetDeviceProcAddr(device, "vkAcquireFullScreenExclusiveModeEXT"));
    if (!acquire) return;

    // Failure (e.g. not focused yet) is fine: it stays a normal fullscreen
    // swapchain, and the next recreate tries again.
    VkResult result = acquire(device, swapchain);
    acquired = result == VK_SUCCESS;
    Log::Info("[WINDOW] Exclusive fullscreen " + std::string(result == VK_SUCCESS ? "acquired" : "not acquired")
        + " (VkResult " + std::to_string(static_cast<int>(result)) + ")");
#else
    (void)device;
    (void)swapchain;
#endif
}

void FullscreenExclusive::Release(VkDevice device, VkSwapchainKHR swapchain)
{
#ifdef _WIN32
    if (!acquired || swapchain == VK_NULL_HANDLE) return;
    acquired = false;

    auto release = reinterpret_cast<PFN_vkReleaseFullScreenExclusiveModeEXT>(
        vkGetDeviceProcAddr(device, "vkReleaseFullScreenExclusiveModeEXT"));
    if (release) release(device, swapchain);
#else
    (void)device;
    (void)swapchain;
#endif
}

} // namespace Volcano
