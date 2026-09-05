#include "GUIController.hpp"
#include <iostream>
#include <chrono>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <GLFW/glfw3.h>
#include "gui/models/GUIComponent.hpp"
#include "gui/models/Button.hpp"

namespace Volcano {

GlobalState* GUIController::state = nullptr;
std::vector<GUI::GUIWindow*> GUIController::windows = {};
ImGuiVulkanContext GUIController::imguiContext;
bool GUIController::initialized = false;
float GUIController::fps = 0.0f;
float GUIController::frameTime = 0.0f;
int GUIController::frameCount = 0;
float GUIController::fpsTimer = 0.0f;
GUI::GUIWindow* GUIController::debugWindow = nullptr;
GUI::GUIComponent* GUIController::debugText = nullptr;

void GUIController::Init(GLFWwindow* window, VkRenderPass renderPass, uint32_t imageCount, GlobalState* globalState)
{
    state = globalState;

    // Initialize ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Initialize ImGui for GLFW
    ImGui_ImplGlfw_InitForVulkan(window, true);

    // Initialize ImGui for Vulkan
    ImGuiVulkan::Init(imguiContext, renderPass, imageCount);

    // Setup style
    ImGui::StyleColorsDark();

    // Create debug window.
    std::string debugWindowName = "Debug";
    debugWindow = CreateWindow(debugWindowName, WindowAlignment::TOP_LEFT);
    debugText = new GUI::GUIComponent(GUI::GUIComponentType::TEXT, "");
    debugWindow->components.push_back(debugText);

    std::cout << "[INFO] GUI Controller initialized with ImGui" << std::endl;
    initialized = true;
}

void GUIController::Shutdown()
{
    if (!initialized) return;

    ImGuiVulkan::Shutdown(imguiContext);
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    std::cout << "[INFO] GUI Controller shutdown" << std::endl;
    initialized = false;
}

void GUIController::NewFrame()
{
    if (!initialized) return;

    ImGui_ImplGlfw_NewFrame();
    ImGuiVulkan::NewFrame(imguiContext);
    ImGui::NewFrame();
}

void GUIController::Update(float deltaTime)
{
    if (!initialized) return;

    // FPS calculation
    frameCount++;
    fpsTimer += deltaTime;

    if (fpsTimer >= 1.0f)
    {
        fps = frameCount / fpsTimer;
        frameTime = (fpsTimer / frameCount) * 1000.0f; // Average frame time in ms
        frameCount = 0;
        fpsTimer = 0.0f;

        // Console FPS output - log every second
        // std::cout << "[DEBUG] FPS: " << fps << " | Frame Time: " << frameTime << "ms" << std::endl;

        // Update debug window text.
        if (debugText) debugText->SetLabel("FPS: " + std::to_string(fps) + " | Frame Time: " + std::to_string(frameTime) + "ms");
    }
}

void GUIController::Render(VkCommandBuffer commandBuffer)
{
    if (!initialized) return;

    /*
    // FPS Display Window
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(180, 80), ImGuiCond_Always);
    ImGui::Begin("Performance", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse);
    ImGui::Text("FPS: %.1f", fps);
    ImGui::Text("Frame Time: %.2f ms", frameTime);
    ImGui::Text("Mouse Input X: %.3f px", state->input->GetAxis("Camera.X"));
    ImGui::Text("Mouse Input Y: %.4f px", state->input->GetAxis("Camera.Y"));
    */

    // Recursively render UI components.
    for (GUI::GUIWindow* window : windows)
    {
        // Avoid SIGSEGV by skipping windows with empty names.
        // Satisfies an assertion in ImGui::Begin at 0x10000000e.
        if (window == nullptr) continue;
        if (window->name.empty()) continue;

        ImGui::SetNextWindowPos(ImVec2(window->positionX, window->positionY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(window->sizeX, window->sizeY), ImGuiCond_Always);
        ImGui::Begin(window->name.c_str(), nullptr,
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse);

        for (GUI::GUIComponent* component : window->components)
        {
            switch (component->GetType())
            {
                default:
                case GUI::GUIComponentType::TEXT: {
                    ImGui::Text("%s", component->GetLabel().c_str());
                    break;
                }
                case GUI::GUIComponentType::BUTTON: {
                    if (ImGui::Button(component->GetLabel().c_str()))
                    {
                        // reinterpret_cast<GUI::Button>(component)->Click();
                        component->Click();
                        std::cout << "[DEBUG] Button named '" << component->GetLabel() << "' clicked." << std::endl;
                    }
                    break;
                }
            }
        }

        ImGui::End();
    }

    ImGui::Render();
    ImGuiVulkan::RenderDrawData(imguiContext, commandBuffer);
}

// Create a new GUI window.
GUI::GUIWindow* GUIController::CreateWindow(const std::string& name, WindowAlignment alignment)
{
    constexpr uint16_t width = 180, height = 80;
    constexpr uint16_t margin = 8;
    uint16_t size[2] = {width, height};
    uint16_t position[2] = {0, 0};

    // WINDOW_WIDTH/HEIGHT reflect the actual (currently fixed, non-resizable)
    // window size, so edges/corners can be computed against it directly.
    switch (alignment)
    {
        default:
        case WindowAlignment::TOP_LEFT: {
            position[0] = margin; position[1] = margin;
            break;
        }
        case WindowAlignment::TOP_RIGHT: {
            position[0] = WINDOW_WIDTH - width - margin; position[1] = margin;
            break;
        }
        case WindowAlignment::BOTTOM_RIGHT: {
            position[0] = WINDOW_WIDTH - width - margin; position[1] = WINDOW_HEIGHT - height - margin;
            break;
        }
        case WindowAlignment::BOTTOM_LEFT: {
            position[0] = margin; position[1] = WINDOW_HEIGHT - height - margin;
            break;
        }
        case WindowAlignment::TOP: {
            position[0] = (WINDOW_WIDTH - width) / 2; position[1] = margin;
            break;
        }
        case WindowAlignment::RIGHT: {
            position[0] = WINDOW_WIDTH - width - margin; position[1] = (WINDOW_HEIGHT - height) / 2;
            break;
        }
        case WindowAlignment::BOTTOM: {
            position[0] = (WINDOW_WIDTH - width) / 2; position[1] = WINDOW_HEIGHT - height - margin;
            break;
        }
        case WindowAlignment::LEFT: {
            position[0] = margin; position[1] = (WINDOW_HEIGHT - height) / 2;
            break;
        }
    }

    GUI::GUIWindow* window = new GUI::GUIWindow{name};
    window->components = {};
    window->positionX = position[0];
    window->positionY = position[1];
    window->sizeX = size[0];
    window->sizeY = size[1];

    windows.push_back(window);
    return window;
}

} // namespace Volcano
