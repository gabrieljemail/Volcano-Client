#include "GUIController.hpp"
#include "Logger.hpp"
#include <chrono>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <GLFW/glfw3.h>
#include "gui/models/GUIComponent.hpp"
#include "gui/models/Button.hpp"
#include "gui/models/Chat.hpp"

namespace Volcano {
// Declared here rather than including network/NetworkClient.hpp: that
// header drags in asio.hpp, which on Windows pulls in <windows.h> —
// whose CreateWindowA/CreateWindow macros collide with
// GUIController::CreateWindow below. This one free function is all
// GUIController actually needs from it.
void SendChatMessage(GlobalState* state, const std::string& message);
}

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
GUI::Screen* GUIController::pauseScreen = nullptr;
bool GUIController::chatInputOpen = false;
bool GUIController::chatInputJustOpened = false;
char GUIController::chatInputBuffer[256] = {};

void GUIController::Init(GLFWwindow* window, VkRenderPass renderPass, uint32_t imageCount, GlobalState* globalState)
{
    state = globalState;
    windowHandle = window;

    // Initialize ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    io.Fonts->AddFontFromFileTTF("resources/fonts/Minecraft.ttf", 16.0f, &fontConfig);

    // TODO: once an icon font (FontAwesome/Material) lands in
    // resources/fonts/, merge its glyphs into the same atlas here with
    // MergeMode = true so icon glyphs share the primary font's baseline,
    // e.g.:
    //   ImFontConfig iconConfig;
    //   iconConfig.MergeMode = true;
    //   iconConfig.GlyphMinAdvanceX = 16.0f;
    //   static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    //   io.Fonts->AddFontFromFileTTF("resources/fonts/FontAwesome.ttf", 16.0f, &iconConfig, iconRanges);

    // Initialize ImGui for GLFW
    ImGui_ImplGlfw_InitForVulkan(window, true);

    // Initialize ImGui for Vulkan
    ImGuiVulkan::Init(imguiContext, renderPass, imageCount);

    // Setup style
    ImGui::StyleColorsDark();
    ApplyVolcanoTheme();

    // Create debug window.
    std::string debugWindowName = "Debug";
    debugWindow = CreateWindow(debugWindowName, WindowAlignment::TOP_LEFT);
    debugText = new GUI::GUIComponent(GUI::GUIComponentType::TEXT, "");
    debugWindow->components.push_back(debugText);

    // Pause screen — Esc opens it whenever no other screen/chat box already
    // owns input (see Update()), Esc closes it again like any other
    // closable screen. This is also the only reliable way to quit on some
    // Windows versions where a stuck cursor-lock/focus fight (see
    // VulkanInit's WindowFocusCallback) can prevent Alt+F4 from ever
    // reaching the OS's own close handling.
    pauseScreen = CreateScreen("Paused", /* closable */ true);
    auto* resumeButton = new GUI::Button("Resume");
    resumeButton->AddClickHandler(new std::function<void()>([] { CloseScreen(); }));
    auto* quitButton = new GUI::Button("Quit Game");
    quitButton->AddClickHandler(new std::function<void()>([] { state->shouldClose = true; }));
    pauseScreen->components.push_back(resumeButton);
    pauseScreen->components.push_back(quitButton);

    Log::Info("[INFO] GUI Controller initialized with ImGui");
    initialized = true;
}

void GUIController::Shutdown()
{
    if (!initialized) return;

    ImGuiVulkan::Shutdown(imguiContext);
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    Log::Info("[INFO] GUI Controller shutdown");
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
    // Esc with nothing else open pauses the game instead, same key Minecraft
    // uses for both. Doesn't fire on top of a non-closable screen (e.g. the
    // connect screen) since that leaves activeScreen non-null and the first
    // branch above already owns Esc in that case.
    else if (activeScreen == nullptr && !chatInputOpen && state->input->IsKeyPressed(GLFW_KEY_ESCAPE))
    {
        OpenScreen(pauseScreen);
    }

    // "T" opens the chat input box, same key Minecraft uses — but not while
    // a Screen already owns input, and not if it's already open (PRESS is
    // edge-triggered so this only fires once per keypress anyway, but the
    // guard makes the intent explicit).
    if (!chatInputOpen && activeScreen == nullptr && state->input->WasActivated("OpenChat"))
    {
        OpenChatInput();
    }
    else if (chatInputOpen && state->input->IsKeyPressed(GLFW_KEY_ESCAPE))
    {
        CloseChatInput();
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
            glm::vec3 position = state->player->GetPosition();
            debugText->SetLabel(
                "FPS: " + std::to_string(fps) +
                "\nFrame Time: " + std::to_string(frameTime) + "ms" +
                "\nPosition X: " + std::to_string(position.x) +
                "\nPosition Y: " + std::to_string(position.y) +
                "\nPosition Z: " + std::to_string(position.z));
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

        RenderChatWindow();
        if (chatInputOpen) RenderChatInputBox();
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
            if (DrawStyledButton(component->GetLabel(), ImVec2(240.0f, 0.0f)))
            {
                component->Click();
                Log::Debug("[DEBUG] Button named '" + component->GetLabel() + "' clicked.");
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
    // No window background — RecordAndSubmitFrame draws the Vulkan 3D scene
    // into this same render pass before GUIController::Render runs, so the
    // world shows through behind the screen's own components.
    ImGui::Begin("##screen", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoBackground);

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

// Read-only, always-on chat/log scrollback — bottom-left, semi-transparent,
// no input capture (there's no chat box to type into yet, just the
// incoming-message + debug-log display). Drawn as a plain ImGui window
// rather than through the GUIComponent/GUIWindow system since it needs
// per-segment colored runs and auto-scroll that GUIWindow's generic
// TEXT/BUTTON/INPUT rendering doesn't support.
// Shared with RenderChatInputBox so the two stay lined up: the input box
// sits in the margin-height strip at the very bottom, and the scrollback
// above it shifts up by exactly that much (+ a little breathing room)
// while chatInputOpen, rather than the two ever overlapping.
namespace { constexpr float CHAT_WIDTH = 480.0f, CHAT_INPUT_HEIGHT = 32.0f; }

void GUIController::RenderChatWindow()
{
    std::vector<GUI::ChatLine> lines = GUI::Chat::GetLines();

    constexpr float height = 220.0f, margin = 8.0f;
    float bottom = static_cast<float>(WINDOW_HEIGHT) - margin
        - (chatInputOpen ? CHAT_INPUT_HEIGHT + margin : 0.0f);
    ImGui::SetNextWindowPos(ImVec2(margin, bottom - height), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(CHAT_WIDTH, height), ImGuiCond_Always);
    // No SetNextWindowBgAlpha override — inherits ImGuiCol_WindowBg from
    // ApplyVolcanoTheme (panelDark, alpha 0.96), same as the debug window.
    ImGui::Begin("##chat", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs);

    for (const GUI::ChatLine& line : lines)
    {
        bool first = true;
        for (const ChatSegment& segment : line.segments)
        {
            if (!first) ImGui::SameLine(0.0f, 0.0f);
            first = false;

            ImVec4 color(
                static_cast<float>((segment.colorRGB >> 16) & 0xFF) / 255.0f,
                static_cast<float>((segment.colorRGB >> 8) & 0xFF) / 255.0f,
                static_cast<float>(segment.colorRGB & 0xFF) / 255.0f,
                1.0f);
            ImGui::TextColored(color, "%s", segment.text.c_str());
        }
    }

    ImGui::SetScrollHereY(1.0f); // keep pinned to the newest line

    ImGui::End();
}

// The actual typeable box, only drawn while chatInputOpen — sits directly
// below RenderChatWindow's (shifted-up) scrollback, in the margin strip it
// vacated. Enter sends the typed text (see GUIController.hpp's own comment
// on IsChatInputOpen) and closes the box.
void GUIController::RenderChatInputBox()
{
    constexpr float margin = 8.0f;
    ImGui::SetNextWindowPos(
        ImVec2(margin, static_cast<float>(WINDOW_HEIGHT) - CHAT_INPUT_HEIGHT - margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(CHAT_WIDTH, CHAT_INPUT_HEIGHT), ImGuiCond_Always);
    ImGui::Begin("##chatinput", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar);

    if (chatInputJustOpened)
    {
        // The same "T" keypress that opened this box would otherwise still
        // be sitting in ImGui's character queue and land as literal text
        // the instant this InputText below gains focus.
        ImGui::GetIO().InputQueueCharacters.resize(0);
        ImGui::SetKeyboardFocusHere();
        chatInputJustOpened = false;
    }

    ImGui::SetNextItemWidth(CHAT_WIDTH - 16.0f);
    if (ImGui::InputText("##chatinputtext", chatInputBuffer, sizeof(chatInputBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
    {
        if (chatInputBuffer[0] != '\0') SendChatMessage(state, chatInputBuffer);
        CloseChatInput();
    }

    ImGui::End();
}

// Dark navy panels with a cyan border tint, laid on top of StyleColorsDark.
// Called once at Init — component-level drawing (DrawStyledButton) pulls
// its own hand-picked colors rather than reading these back, since ImGui's
// built-in widget colors don't cover custom-drawn glow/border fills.
void GUIController::ApplyVolcanoTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    const ImVec4 panelDark  = ImVec4(0.04f, 0.06f, 0.10f, 0.96f);
    const ImVec4 panelMid   = ImVec4(0.07f, 0.10f, 0.16f, 0.96f);
    const ImVec4 panelLight = ImVec4(0.10f, 0.14f, 0.21f, 1.00f);
    const ImVec4 cyan       = ImVec4(0.20f, 0.85f, 0.95f, 1.00f);
    const ImVec4 cyanDim    = ImVec4(0.20f, 0.85f, 0.95f, 0.35f);
    const ImVec4 cyanHover  = ImVec4(0.35f, 0.92f, 1.00f, 0.55f);

    colors[ImGuiCol_WindowBg]         = panelDark;
    colors[ImGuiCol_ChildBg]          = panelDark;
    colors[ImGuiCol_PopupBg]          = panelDark;
    colors[ImGuiCol_Border]           = cyanDim;
    colors[ImGuiCol_BorderShadow]     = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg]          = panelMid;
    colors[ImGuiCol_FrameBgHovered]   = panelLight;
    colors[ImGuiCol_FrameBgActive]    = panelLight;
    colors[ImGuiCol_TitleBg]          = panelDark;
    colors[ImGuiCol_TitleBgActive]    = panelDark;
    colors[ImGuiCol_TitleBgCollapsed] = panelDark;
    colors[ImGuiCol_Button]           = panelMid;
    colors[ImGuiCol_ButtonHovered]    = panelLight;
    colors[ImGuiCol_ButtonActive]     = cyanDim;
    colors[ImGuiCol_CheckMark]        = cyan;
    colors[ImGuiCol_SliderGrab]       = cyan;
    colors[ImGuiCol_SliderGrabActive] = cyanHover;
    colors[ImGuiCol_Separator]        = cyanDim;
    colors[ImGuiCol_SeparatorHovered] = cyanHover;
    colors[ImGuiCol_Text]             = ImVec4(0.92f, 0.96f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled]     = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
}

// See header: flat-glow button drawn with ImDrawList so it doesn't depend
// on ImGui's built-in button skin. An InvisibleButton underneath handles
// click/hover/active state; everything visible is drawn manually on top.
bool GUIController::DrawStyledButton(const std::string& label, ImVec2 size)
{
    if (size.x <= 0.0f) size.x = ImGui::CalcTextSize(label.c_str()).x + 32.0f;
    if (size.y <= 0.0f) size.y = ImGui::GetFrameHeight() + 8.0f;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label.c_str(), size);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 rectMax = ImVec2(pos.x + size.x, pos.y + size.y);
    constexpr float rounding = 6.0f;

    ImU32 fill = active
        ? IM_COL32(20, 60, 70, 255)
        : hovered
            ? IM_COL32(16, 48, 58, 255)
            : IM_COL32(12, 32, 40, 235);
    ImU32 border = (hovered || active)
        ? IM_COL32(90, 235, 255, 255)
        : IM_COL32(60, 170, 190, 160);

    // Soft outer glow on hover/active — a slightly larger, low-alpha rect
    // behind the button, since ImGui has no native blur/shadow primitive.
    if (hovered || active)
    {
        drawList->AddRectFilled(
            ImVec2(pos.x - 3.0f, pos.y - 3.0f),
            ImVec2(rectMax.x + 3.0f, rectMax.y + 3.0f),
            IM_COL32(60, 220, 255, active ? 60 : 35), rounding + 3.0f);
    }

    drawList->AddRectFilled(pos, rectMax, fill, rounding);
    drawList->AddRect(pos, rectMax, border, rounding, 0, 1.5f);

    ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
    ImVec2 textPos = ImVec2(
        pos.x + (size.x - textSize.x) * 0.5f,
        pos.y + (size.y - textSize.y) * 0.5f);
    drawList->AddText(textPos, IM_COL32(235, 248, 255, 255), label.c_str());

    return clicked;
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
        // Only re-capture the cursor if the window is actually focused right
        // now. Locking unconditionally here caused a Windows-only feedback
        // loop: closing a screen while the window was unfocused (e.g. right
        // after alt-tabbing away) would re-clip the cursor to the window,
        // which then fought with the OS over focus/mouse ownership. The
        // window's own focus callback (see VulkanInit's WindowFocusCallback)
        // is responsible for locking the cursor once real focus returns.
#ifdef _WIN32
        if (glfwGetWindowAttrib(windowHandle, GLFW_FOCUSED))
#endif
        {
            glfwSetInputMode(windowHandle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }
}

// Raise the chat input box and free the cursor, same reasoning OpenScreen
// gives for doing so — mouse-look shouldn't fight with typing, and
// RenderThread::PollInputs is the piece that actually stops reading
// movement/camera input while this is open (see its own IsChatInputOpen()
// check, right alongside the existing IsScreenOpen() one).
void GUIController::OpenChatInput()
{
    chatInputOpen = true;
    chatInputJustOpened = true;
    chatInputBuffer[0] = '\0';
    if (windowHandle != nullptr)
    {
        glfwSetInputMode(windowHandle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}

// Close the chat input box and re-capture the cursor for the fly-cam —
// same focus caveat CloseScreen's own comment explains (only re-lock if
// the window is actually focused right now).
void GUIController::CloseChatInput()
{
    chatInputOpen = false;
    chatInputJustOpened = false;
    if (windowHandle != nullptr)
    {
#ifdef _WIN32
        if (glfwGetWindowAttrib(windowHandle, GLFW_FOCUSED))
#endif
        {
            glfwSetInputMode(windowHandle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }
}

} // namespace Volcano
