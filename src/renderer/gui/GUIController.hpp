#pragma once
#ifndef GUI_CONTROLLER_H
#define GUI_CONTROLLER_H

#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <imgui.h>
#include "../VulkanInit.hpp"
#include "ImGuiVulkan.hpp"
#include "GlobalState.hpp"
#include "models/GUIWindow.hpp"
#include "models/Screen.hpp"

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

    // Screen API:
    // Create a fullscreen, Minecraft-like menu screen. It isn't shown until
    // passed to OpenScreen.
    static GUI::Screen* CreateScreen(const std::string& title, bool closable = true);
    // Show the given screen fullscreen, hiding every other window (e.g. the
    // debug HUD) and the game's own inputs until it closes. Also releases
    // the mouse cursor so ImGui can drive it.
    static void OpenScreen(GUI::Screen* screen);
    // Close whatever screen is currently open (no-op if none is), restoring
    // the HUD, game inputs, and captured mouse cursor.
    static void CloseScreen();
    static bool IsScreenOpen() { return activeScreen != nullptr; }

    // Chat input box — "T" (see VolcanoClient.cpp's "OpenChat" action)
    // raises it; Enter sends the typed text (via NetworkClient::
    // SendChatMessage, a no-op if there's no live connection) and closes
    // it, Escape closes it without sending. Unlike a Screen this doesn't
    // hide the HUD or the chat scrollback (which shifts up to make room
    // for the input box instead) — it only needs to steal keyboard focus
    // and stop the mouse-look/movement inputs RenderThread::PollInputs
    // would otherwise also apply to whatever's being typed.
    static bool IsChatInputOpen() { return chatInputOpen; }
    static void OpenChatInput();
    static void CloseChatInput();

    // Draws a flat-glow button in the Volcano theme (filled rect + cyan
    // border, brighter on hover/press) instead of ImGui's built-in button
    // skin. Public/reusable so future custom widgets — nav rail, server
    // list rows, partner cards — can share the same look. size.x/y <= 0
    // auto-sizes that axis to fit the label.
    static bool DrawStyledButton(const std::string& label, ImVec2 size = ImVec2(0.0f, 0.0f));

private:
    static GlobalState* state;
    static GLFWwindow* windowHandle;
    static std::vector<GUI::GUIWindow*> windows;
    static GUI::Screen* activeScreen;
    static ImGuiVulkanContext imguiContext;
    static bool initialized;
    static float fps;
    static float frameTime;
    static int frameCount;
    static float fpsTimer;
    static GUI::GUIWindow* debugWindow;
    static GUI::GUIComponent* debugText;
    static GUI::Screen* pauseScreen;

    static bool chatInputOpen;
    static bool chatInputJustOpened; // Consumed once by RenderChatInputBox to grab keyboard focus the frame it opens.
    static char chatInputBuffer[256];

    static void RenderComponent(GUI::GUIComponent* component);
    static void RenderScreen(GUI::Screen& screen);
    static void RenderChatWindow();
    static void RenderChatInputBox();
    static void ApplyVolcanoTheme();
};

} // namespace Volcano

#endif
