#pragma once
#ifndef GUI_CONTROLLER_H
#define GUI_CONTROLLER_H

#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "../VulkanInit.hpp"
#include "ImGuiVulkan.hpp"
#include "GlobalState.hpp"
#include "models/GUIWindow.hpp"

namespace Volcano {

enum class WindowAlignment : uint8_t {
    TOP_LEFT = 0,
    TOP_RIGHT = 1,
    BOTTOM_RIGHT = 2,
    BOTTOM_LEFT = 3,
    TOP = 4,
    RIGHT = 5,
    BOTTOM = 6,
    LEFT = 7
};

class GUIController {
public:
    static void Init(GLFWwindow* window, VkRenderPass renderPass, uint32_t imageCount, GlobalState* globalState);
    static void Shutdown();
    static void NewFrame();
    static void Update(float deltaTime);
    static void Render(VkCommandBuffer commandBuffer);

    static float GetFPS() { return fps; }
    static float GetFrameTime() { return frameTime; }

    // API:
    // Render a window.
    static GUI::GUIWindow* CreateWindow(const std::string& name, WindowAlignment alignment);

private:
    static GlobalState* state;
    static std::vector<GUI::GUIWindow*> windows;
    static ImGuiVulkanContext imguiContext;
    static bool initialized;
    static float fps;
    static float frameTime;
    static int frameCount;
    static float fpsTimer;
    static GUI::GUIWindow* debugWindow;
    static GUI::GUIComponent* debugText;
};

} // namespace Volcano

#endif
