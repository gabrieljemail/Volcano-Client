#include <thread>
#include <memory>
#include <iostream>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>
#include "VulkanInit.hpp"
#include "RenderThread.hpp"

using namespace std;

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

// Main function:
void Init()
{
    // Make an instance.
    vkb::InstanceBuilder instanceBuilder;
    auto instanceReturn = instanceBuilder
        .set_app_name(APP_NAME)
        .set_app_version(APP_VERSION)
        .set_engine_name(ENGINE_NAME)
        .set_engine_version(ENGINE_VERSION)
        .require_api_version(1,0,0)
        .build();
    
    if (!instanceReturn)
    {
        cerr << "[ERROR] Failed to create Vulkan instance: " << instanceReturn.error().message() << endl;
        Cleanup();
        exit(2);
        return;
    }
    instance = instanceReturn.value();

    // Get a device.
    vkb::PhysicalDeviceSelector physDeviceSelector(instance);
    auto physDeviceSelectorReturn = physDeviceSelector
        .select(); // TODO: Read settings.json if the user has a preferred device name.
    
    if (!physDeviceSelectorReturn)
    {
        cerr << "[ERROR] No suitable Vulkan devices." << endl;
        Cleanup();
        exit(3);
        return;
    }
    physicalDevice = physDeviceSelectorReturn.value();

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

    // Initialize GLFW.
    if (!glfwInit())
    {
        cerr << "[ERROR] Failed to initialize GLFW." << endl;
        Cleanup();
        exit(5);
        return;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Prevent GLFW from creating an OpenGL context.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE); // Prevent resizing the window.
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE); // Hide system window decorations.

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

    // Create the surface.
    VkResult error = glfwCreateWindowSurface(instance, window, nullptr, &surface);
    if (error != VK_SUCCESS)
    {
        cerr << "[ERROR] Failed to create window surface." << endl;
        glfwTerminate();
        Cleanup();
        exit(7);
        return;
    }

    // Create the swapchan.
    vkb::SwapchainBuilder swapchainBuilder{device, surface};
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
}

void Cleanup()
{
    if (device != VK_NULL_HANDLE)
    {
        vkb::destroy_device(device);
    }

    if (instance != VK_NULL_HANDLE)
    {
        vkb::destroy_instance(instance);
    }

    if (surface != VK_NULL_HANDLE)
    {
        vkb::destroy_surface(instance, surface);
    }

    if (swapchain != VK_NULL_HANDLE)
    {
        vkb::destroy_swapchain(swapchain);
    }
}

// Getters:
VkDevice GetDevice()
{
    return device;
}

// Setters: