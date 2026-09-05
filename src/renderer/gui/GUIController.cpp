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
GLFWwindow* GUIController::windowHandle = nullptr;
std::vector<GUI::GUIWindow*> GUIController::windows = {};
GUI::Screen* GUIController::activeScreen = nullptr;
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
    windowHandle = window;

    // Initialize ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // The default font (ProggyClean) is a hand-drawn bitmap font that's
    // deliberately not anti-aliased. Swap in a real TrueType font — its
    // glyphs are rasterized with actual alpha-coverage antialiasing — so
    // menu/HUD text doesn't look jagged. This only affects the font atlas,
    // not the world renderer's pipeline/render pass.
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    io.Fonts->AddFontFromFileTTF("resources/fonts/DroidSans.ttf", 16.0f, &fontConfig);

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

    // Esc closes whatever screen is open, same as Minecraft's menus.
    if (activeScreen != nullptr && activeScreen->closable && state->input->IsKeyPressed(GLFW_KEY_ESCAPE))
    {
        CloseScreen();
    }

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
        if (debugText)
        {
            float mouseX = state->input->GetAxis("Camera.X");
            float mouseY = state->input->GetAxis("Camera.Y");
            debugText->SetLabel(
                "FPS: " + std::to_string(fps) +
                "\nFrame Time: " + std::to_string(frameTime) + "ms" +
                "\nMouse X: " + std::to_string(mouseX) +
                "\nMouse Y: " + std::to_string(mouseY));
        }
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

    // A screen (e.g. the connect screen) takes over the whole frame — it
    // hides every other window, including the debug HUD, while it's open.
    if (activeScreen != nullptr)
    {
        RenderScreen(*activeScreen);
    }
    else
    {
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
                RenderComponent(component);
            }

            ImGui::End();
        }
    }

    ImGui::Render();
    ImGuiVulkan::RenderDrawData(imguiContext, commandBuffer);
}

// Render a single component the same way regardless of whether it lives in
// a HUD window or a fullscreen Screen.
void GUIController::RenderComponent(GUI::GUIComponent* component)
{
    if (component == nullptr || !component->IsVisible()) return;

    switch (component->GetType())
    {
        default:
        case GUI::GUIComponentType::TEXT: {
            ImGui::Text("%s", component->GetLabel().c_str());
            break;
        }
        case GUI::GUIComponentType::BUTTON: {
            if (ImGui::Button(component->GetLabel().c_str(), ImVec2(240.0f, 0.0f)))
            {
                component->Click();
                std::cout << "[DEBUG] Button named '" << component->GetLabel() << "' clicked." << std::endl;
            }
            break;
        }
        case GUI::GUIComponentType::INPUT: {
            char* buffer = component->GetTextBuffer();
            if (buffer != nullptr)
            {
                ImGui::SetNextItemWidth(240.0f);
                ImGui::InputText(component->GetLabel().c_str(), buffer, component->GetTextBufferCapacity());
            }
            break;
        }
    }
}

// Draw the active Screen fullscreen, title first, then its components
// stacked and centered — the ImGui equivalent of Minecraft's GuiScreen.
void GUIController::RenderScreen(GUI::Screen& screen)
{
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(WINDOW_WIDTH), static_cast<float>(WINDOW_HEIGHT)), ImGuiCond_Always);
    ImGui::Begin("##screen", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImVec2 titleSize = ImGui::CalcTextSize(screen.title.c_str());
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - titleSize.x) * 0.5f, 24.0f));
    ImGui::Text("%s", screen.title.c_str());

    ImGui::Dummy(ImVec2(0.0f, 16.0f));

    constexpr float contentWidth = 240.0f;
    for (GUI::GUIComponent* component : screen.components)
    {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - contentWidth) * 0.5f);
        RenderComponent(component);
    }

    ImGui::End();
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

// Create a fullscreen menu screen. Not shown until passed to OpenScreen.
GUI::Screen* GUIController::CreateScreen(const std::string& title, bool closable)
{
    GUI::Screen* screen = new GUI::Screen{title};
    screen->components = {};
    screen->closable = closable;
    return screen;
}

// Show a screen fullscreen, hiding the HUD and releasing the mouse cursor
// so ImGui (rather than the fly-cam) drives it.
void GUIController::OpenScreen(GUI::Screen* screen)
{
    activeScreen = screen;
    if (windowHandle != nullptr)
    {
        glfwSetInputMode(windowHandle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}

// Close whatever screen is open, restoring the HUD and re-capturing the
// mouse cursor for the fly-cam.
void GUIController::CloseScreen()
{
    activeScreen = nullptr;
    if (windowHandle != nullptr)
    {
        glfwSetInputMode(windowHandle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }
}

} // namespace Volcano
