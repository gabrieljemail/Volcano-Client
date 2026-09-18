#include "GUIController.hpp"
#include "Logger.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>
#include <cstring>
#include <stb_image.h>
#include "gui/models/GUIComponent.hpp"
#include "gui/models/Button.hpp"
#include "gui/models/Chat.hpp"
#include "inventory/ItemRegistry.hpp"

namespace Volcano {
// Declared here rather than including network/NetworkClient.hpp: that
// header drags in asio.hpp, which on Windows pulls in <windows.h> —
// whose CreateWindowA/CreateWindow macros collide with
// GUIController::CreateWindow below. This one free function is all
// GUIController actually needs from it.
void SendChatMessage(GlobalState* state, const std::string& message);
void SendChatCommand(GlobalState* state, const std::string& command);
void SendRespawnRequest(GlobalState* state);
}

namespace Volcano {

GlobalState* GUIController::state = nullptr;
GLFWwindow* GUIController::windowHandle = nullptr;
const TextureManager* GUIController::textureManager = nullptr;
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
GUI::Screen* GUIController::deathScreen = nullptr;
GUI::GUIComponent* GUIController::deathMessageText = nullptr;
GUI::Screen* GUIController::inventoryScreen = nullptr;
bool GUIController::chatInputOpen = false;
bool GUIController::chatInputJustOpened = false;
char GUIController::chatInputBuffer[256] = {};
std::unordered_map<uint16_t, GUIController::ItemIcon> GUIController::itemIconCache;
GUIController::StandaloneIcon GUIController::crosshairIcon;
GUIController::StandaloneIcon GUIController::crosshairEntityIcon;

void GUIController::Init(GLFWwindow* window, VkRenderPass renderPass, uint32_t imageCount, GlobalState* globalState,
                          const TextureManager* textureMgr)
{
    state = globalState;
    windowHandle = window;
    textureManager = textureMgr;

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
    debugWindow = CreateWindow(debugWindowName, GUI::ScreenAnchor::TOP_RIGHT);
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

    // Death screen — opened by Update() when GlobalState::playerDied is set
    // (see NetworkClient's PlayerCombatKill handling). Not closable via Esc;
    // Respawn is the only way out.
    deathScreen = CreateScreen("You Died!", /* closable */ false);
    deathMessageText = new GUI::GUIComponent(GUI::GUIComponentType::TEXT, "");
    auto* respawnButton = new GUI::Button("Respawn");
    respawnButton->AddClickHandler(new std::function<void()>([] {
        SendRespawnRequest(state);
        CloseScreen();
    }));
    deathScreen->components.push_back(deathMessageText);
    deathScreen->components.push_back(respawnButton);

    // Inventory screen — "E" toggles it (see Update()), same as vanilla.
    // customContent does all the actual drawing (a raw-ImDrawList slot
    // grid, not the generic TEXT/BUTTON/INPUT stack) — see
    // RenderInventoryScreen's own comment.
    inventoryScreen = CreateScreen("Inventory", /* closable */ true);
    inventoryScreen->customContent = RenderInventoryScreen;

    // Crosshair — see RenderCrosshair(). Loaded once here rather than
    // lazily on first draw (like GetItemIcon's item-icon cache) since,
    // unlike item icons, both variants are needed from the very first HUD
    // frame and there are only ever exactly two of them.
    crosshairIcon = LoadStandaloneIcon("resources/crosshair.png");
    crosshairEntityIcon = LoadStandaloneIcon("resources/crosshair-entity.png");

    Log::Info("[INFO] GUI Controller initialized with ImGui");
    initialized = true;
}

void GUIController::Shutdown()
{
    if (!initialized) return;

    // Item icon descriptor sets are allocated from ImGuiVulkan's own
    // descriptor pool (ImGui_ImplVulkan_AddTexture, via ImGuiContext's
    // DescriptorPool — see ImGuiVulkan::Init's init_info.DescriptorPool),
    // so destroying that pool below (inside ImGuiVulkan::Shutdown) already
    // implicitly frees them; only the single-layer VkImageViews
    // GetItemIcon() created are this class's own resources, destroyed
    // after the descriptor sets referencing them no longer matter.
    ImGuiVulkan::Shutdown(imguiContext);

    for (auto& [layer, icon] : itemIconCache) {
        if (icon.view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), icon.view, nullptr);
    }
    itemIconCache.clear();

    // Unlike itemIconCache's views above, these own the whole image (see
    // StandaloneIcon's own comment) — the view alone isn't enough to free.
    for (StandaloneIcon* icon : { &crosshairIcon, &crosshairEntityIcon }) {
        if (icon->view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), icon->view, nullptr);
        if (icon->image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, icon->image, icon->allocation);
        *icon = StandaloneIcon{};
    }

    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    Log::Info("[INFO] GUI Controller shutdown");
    initialized = false;
}

// See the header's own comment. Returns a null ImTextureID (0) on any
// Vulkan failure — callers skip drawing rather than pass that to AddImage.
ImTextureID GUIController::GetItemIcon(uint16_t textureLayer)
{
    auto cached = itemIconCache.find(textureLayer);
    if (cached != itemIconCache.end()) {
        return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(cached->second.descriptorSet));
    }

    ItemIcon icon;

    // A narrow 2D slice of TextureManager::array's single 2D_ARRAY image —
    // same image, same format, just one layer/one mip, since ImGui's
    // Vulkan backend draws a single flat texture per ImTextureID rather
    // than sampling an indexed array layer the way the 3D world pass does.
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = textureManager->array.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB; // Matches TextureManager::CreateArrayImage's own format.
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1; // Icons are drawn small; no need for the world pass's full mip chain.
    viewInfo.subresourceRange.baseArrayLayer = textureLayer;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &icon.view) != VK_SUCCESS) {
        Log::Error("[ERROR] GUIController: failed to create an item icon view for texture layer "
            + std::to_string(textureLayer));
        return 0;
    }

    icon.descriptorSet = ImGui_ImplVulkan_AddTexture(icon.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    itemIconCache[textureLayer] = icon;
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(icon.descriptorSet));
}

ImTextureID GUIController::IconForItem(const ItemStack& item)
{
    if (item.IsEmpty()) return 0;
    const ItemRegistry::ItemTypeInfo* info = ItemRegistry::Lookup(item.itemId);
    if (!info || !info->hasTexture) return 0;
    return GetItemIcon(info->textureLayer);
}

// See the header's own comment.
GUIController::StandaloneIcon GUIController::LoadStandaloneIcon(const std::string& path)
{
    StandaloneIcon icon;

    int width = 0, height = 0, channels = 0;
    uint8_t* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        Log::Error("[ERROR] GUIController: failed to load " + path + ", skipping.");
        return icon;
    }

    // Single 2D image, one layer, no mip chain — this is small UI art drawn
    // at roughly its native size, not a world texture sampled at a range of
    // distances the way TextureManager's array needs mips for.
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(vmaAllocator, &imageInfo, &allocInfo, &icon.image, &icon.allocation, nullptr) != VK_SUCCESS) {
        Log::Error("[ERROR] GUIController: failed to create image for " + path);
        stbi_image_free(pixels);
        return StandaloneIcon{};
    }

    // Upload via the same staging-buffer-then-copy path TextureManager::
    // UploadLayer uses (CreateBuffer/BeginOneShotCommands/
    // TransitionImageLayout — see VulkanInit.hpp).
    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
    AllocatedBuffer staging = CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);

    void* mapped = nullptr;
    vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
    std::memcpy(mapped, pixels, static_cast<size_t>(size));
    vmaUnmapMemory(vmaAllocator, staging.allocation);
    stbi_image_free(pixels);

    VkCommandBuffer cmd = BeginOneShotCommands();
    TransitionImageLayout(cmd, icon.image, 0, 1, 0, 1,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    vkCmdCopyBufferToImage(cmd, staging.buffer, icon.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    TransitionImageLayout(cmd, icon.image, 0, 1, 0, 1,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneShotCommands(cmd);

    vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = icon.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &icon.view) != VK_SUCCESS) {
        Log::Error("[ERROR] GUIController: failed to create image view for " + path);
        vmaDestroyImage(vmaAllocator, icon.image, icon.allocation);
        return StandaloneIcon{};
    }

    icon.descriptorSet = ImGui_ImplVulkan_AddTexture(icon.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return icon;
}

// Centered on the display, drawn on ImGui's foreground draw list rather
// than in a Begin/End window — it needs no chrome, background, or input
// handling, just an image pinned to the exact center every frame regardless
// of display size (unlike the hotbar/status HUD, which anchors off
// HotbarOrigin()).
void GUIController::RenderCrosshair()
{
    // Native size of both source PNGs; drawn 1:1 rather than scaled by a
    // Graphics.GUIScale-style setting, since this client has none yet.
    constexpr float CROSSHAIR_SIZE = 32.0f;

    const StandaloneIcon& active = state->lookingAtEntity.load() ? crosshairEntityIcon : crosshairIcon;
    if (active.descriptorSet == VK_NULL_HANDLE) return; // Missing/failed-to-load asset — draw nothing.

    ImTextureID texture = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(active.descriptorSet));
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 center(display.x * 0.5f, display.y * 0.5f);
    ImVec2 half(CROSSHAIR_SIZE * 0.5f, CROSSHAIR_SIZE * 0.5f);

    ImGui::GetForegroundDrawList()->AddImage(texture,
        ImVec2(center.x - half.x, center.y - half.y),
        ImVec2(center.x + half.x, center.y + half.y));
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

    // Death takes over whatever's on screen — including the pause menu, if
    // that happened to be open — same as vanilla's death screen does.
    if (state->playerDied.exchange(false))
    {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(state->deathMutex);
            message = state->deathMessage;
        }
        deathMessageText->SetLabel(message);
        OpenScreen(deathScreen);
    }

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

    // "E" toggles the inventory screen, same key Minecraft uses — opens it
    // only while nothing else owns input (matching "T" above), closes it
    // again on a second press. Esc also closes it via the generic
    // activeScreen->closable branch above, same as any other closable
    // screen — no separate handling needed for that direction.
    if (activeScreen == nullptr && !chatInputOpen && state->input->WasActivated("ToggleInventory"))
    {
        OpenScreen(inventoryScreen);
    }
    else if (activeScreen == inventoryScreen && state->input->WasActivated("ToggleInventory"))
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

        // GlobalState::presentMode is the swapchain's actual active
        // VkPresentModeKHR (see its own comment) — this client only ever
        // requests FIFO or IMMEDIATE (Graphics.VSync), but reports whatever
        // vk-bootstrap actually granted, so an unsupported request showing
        // up here as something else is visible rather than silently wrong.
        std::string vsync;
        switch (static_cast<VkPresentModeKHR>(state->presentMode))
        {
            case VK_PRESENT_MODE_FIFO_KHR: vsync = "VSync (FIFO)"; break;
            case VK_PRESENT_MODE_IMMEDIATE_KHR: vsync = "Immediate"; break;
            case VK_PRESENT_MODE_MAILBOX_KHR: vsync = "Mailbox"; break;
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: vsync = "FIFO Relaxed"; break;
            default: vsync = "Unknown"; break;
        }

        // Update debug window text.
        if (debugText)
        {
            glm::vec3 position = state->player->GetPosition();

            // Live framebuffer size, not GlobalState::resolution (the
            // startup-config value) — glfwGetFramebufferSize is safe here
            // since this runs on the same GLFW-window-owning thread every
            // other window query in this codebase requires.
            int windowWidth = 0, windowHeight = 0;
            glfwGetFramebufferSize(state->window, &windowWidth, &windowHeight);

            const SystemInfo& sys = state->systemInfo;
            std::string vulkanVersion = "Vulkan "
                + std::to_string(VK_API_VERSION_MAJOR(sys.vulkanApiVersion)) + "."
                + std::to_string(VK_API_VERSION_MINOR(sys.vulkanApiVersion)) + "."
                + std::to_string(VK_API_VERSION_PATCH(sys.vulkanApiVersion));

            debugText->SetLabel(
                std::string("Debug Info") +
                "\nFPS: " + std::to_string(static_cast<int>(std::round(fps))) +
                "\nFrame Time: " + std::to_string(frameTime) + "ms" +
                "\nPresent Mode: " + vsync +
                // No "<N>x" unit-count prefix on GPU, unlike CPU — see
                // SystemInfo's own comment on why there's no portable
                // cross-vendor way to get an EU/CU/SM-style count.
                "\nCPU: " + std::to_string(sys.cpuLogicalCores) + "x " + sys.cpuModel +
                "\nGPU: " + sys.gpuModel +
                "\nDisplay: " + std::to_string(windowWidth) + "x" + std::to_string(windowHeight)
                    + " (" + sys.gpuDriverVendor + ")" +
                "\nGraphics API: " + vulkanVersion);
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

            ImVec2 size(window->sizeX, window->sizeY);
            ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse;

            // Debug overlay only, deliberately: 2x width / 3x height of
            // whatever it'd naturally auto-fit to — a dev-only convenience
            // (easier to read/screenshot while this is being built out) on
            // top of the real content, not a real layout choice, and not
            // something any other window here should inherit. AlwaysAutoResize
            // would just override any size given to it, so this computes
            // the natural fit itself (CalcTextSize + the style's own window
            // padding — the same thing AlwaysAutoResize would land on) and
            // multiplies THAT instead of guessing a fixed pixel size.
            if (window == debugWindow && debugText != nullptr)
            {
                ImVec2 natural = ImGui::CalcTextSize(debugText->GetLabel().c_str());
                const ImGuiStyle& style = ImGui::GetStyle();
                natural.x += style.WindowPadding.x * 2.0f;
                natural.y += style.WindowPadding.y * 2.0f;
                size = ImVec2(natural.x * 2.0f, natural.y * 3.0f);
                flags &= ~ImGuiWindowFlags_AlwaysAutoResize;
            }

            ImGui::SetNextWindowPos(ResolveAnchor(window->anchor, size, ImVec2(window->offsetX, window->offsetY)), ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            ImGui::Begin(window->name.c_str(), nullptr, flags);

            for (GUI::GUIComponent* component : window->components)
            {
                RenderComponent(component);
            }

            ImGui::End();
        }

        RenderChatWindow();
        if (chatInputOpen) RenderChatInputBox();
        if (state->input->IsKeyDown(GLFW_KEY_TAB)) RenderPlayerListWindow();

        // Hotbar/armor first — they define HotbarOrigin(), which the status
        // bars and movement-state panel both position themselves off of.
        RenderHotbarAndArmor();
        RenderPlayerStatusBars();
        RenderMovementStatePanel();
        RenderCrosshair();
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
    // The live display size (kept in sync with the real framebuffer every
    // frame), not the launch-time Window.Width/Height config — otherwise a
    // resize or F11 fullscreen leaves this covering only the old size.
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
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

    // Vanilla-style menu darkening. The world keeps rendering behind every
    // Screen (that's the whole point of NoBackground above), so without
    // this a pause/connect screen reads as translucent UI floating over a
    // fully lit 3D view rather than Minecraft's dimmed-backdrop menus. Drawn
    // as the very first thing on this window's own draw list (before the
    // title/components below) so everything else layers on top of it.
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 140));

    ImVec2 titleSize = ImGui::CalcTextSize(screen.title.c_str());
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - titleSize.x) * 0.5f, 24.0f));
    ImGui::Text("%s", screen.title.c_str());

    ImGui::Dummy(ImVec2(0.0f, 16.0f));

    // customContent (see Screen::customContent's own comment) replaces the
    // generic centered TEXT/BUTTON/INPUT stack entirely for a screen that
    // needs its own layout — the inventory screen's slot grid, so far.
    if (screen.customContent)
    {
        screen.customContent();
    }
    else
    {
        constexpr float contentWidth = 240.0f;
        for (GUI::GUIComponent* component : screen.components)
        {
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - contentWidth) * 0.5f);
            RenderComponent(component);
        }
    }

    ImGui::End();
}

// Read-only, always-on chat/log scrollback — top-left, mostly transparent
// (unlike every other panel — see the SetNextWindowBgAlpha override below),
// no input capture (there's no chat box to type into yet, just the
// incoming-message + debug-log display). Drawn as a plain ImGui window
// rather than through the GUIComponent/GUIWindow system since it needs
// per-segment colored runs and auto-scroll that GUIWindow's generic
// TEXT/BUTTON/INPUT rendering doesn't support.
// Shared with RenderChatInputBox so the two stay lined up: the scrollback
// sits fixed at the top margin, and the input box (when chatInputOpen)
// appends directly below it rather than the two ever overlapping.
namespace {
    constexpr float CHAT_WIDTH = 480.0f, CHAT_INPUT_HEIGHT = 32.0f;
    constexpr float CHAT_HEIGHT = 220.0f, CHAT_MARGIN = 8.0f, CHAT_GAP = 4.0f;
}

void GUIController::RenderChatWindow()
{
    std::vector<GUI::ChatLine> lines = GUI::Chat::GetLines();

    // Anchored to the top-left of the *current* display, not a stale
    // Window.Height config value — previously this used the configured
    // launch resolution regardless of the actual window size, so toggling
    // F11 into fullscreen (a much taller real display) left the chat
    // window positioned according to the wrong height.
    ImVec2 pos = ResolveAnchor(GUI::ScreenAnchor::TOP_LEFT, ImVec2(CHAT_WIDTH, CHAT_HEIGHT), ImVec2(CHAT_MARGIN, CHAT_MARGIN));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(CHAT_WIDTH, CHAT_HEIGHT), ImGuiCond_Always);
    // Explicit low alpha override — chat should stay mostly see-through so
    // it doesn't block the world behind it, unlike every other panel, which
    // all inherit ApplyVolcanoTheme's far more opaque ImGuiCol_WindowBg
    // (panelDark, alpha 0.96 — see the debug window for that default).
    ImGui::SetNextWindowBgAlpha(0.25f);
    // NoBringToFrontOnFocus pins this to draw-call order (called first in
    // Render(), before the hotbar/status/movement-state HUD) rather than
    // letting ImGui's own window stack reorder it to the top — without this
    // the scrollback could end up drawn OVER the center HUD instead of
    // staying behind it.
    ImGui::Begin("##chat", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

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
// below RenderChatWindow's scrollback, which stays fixed in place rather
// than moving to make room for it. Enter sends the typed text (see
// GUIController.hpp's own comment on IsChatInputOpen) and closes the box.
void GUIController::RenderChatInputBox()
{
    ImVec2 pos = ResolveAnchor(GUI::ScreenAnchor::TOP_LEFT, ImVec2(CHAT_WIDTH, CHAT_INPUT_HEIGHT),
        ImVec2(CHAT_MARGIN, CHAT_MARGIN + CHAT_HEIGHT + CHAT_GAP));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
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
        if (chatInputBuffer[0] == '/') SendChatCommand(state, chatInputBuffer + 1);
        else if (chatInputBuffer[0] != '\0') SendChatMessage(state, chatInputBuffer);
        CloseChatInput();
    }

    ImGui::End();
}

// Vanilla-style Tab-list overlay — just names, no ping/gamemode columns (see
// GlobalState::playerList's own comment for where the data comes from).
// Only called while Tab is held (see Render()); NoInputs so it never steals
// mouse focus from whatever's under it.
void GUIController::RenderPlayerListWindow()
{
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(state->playerListMutex);
        names.reserve(state->playerList.size());
        for (auto& [uuid, name] : state->playerList) names.push_back(name);
    }
    std::sort(names.begin(), names.end());

    constexpr float width = 220.0f, lineHeight = 20.0f, padding = 16.0f;
    float height = padding + static_cast<float>(names.size()) * lineHeight;
    ImVec2 pos = ResolveAnchor(GUI::ScreenAnchor::TOP, ImVec2(width, height), ImVec2(0.0f, 40.0f));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::Begin("##playerlist", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs);

    for (const std::string& name : names) ImGui::Text("%s", name.c_str());

    ImGui::End();
}

// Hotbar/armor/status-bar/movement-state geometry — kept together so the
// four HUD pieces that key off "where's the hotbar" (RenderHotbarAndArmor,
// RenderPlayerStatusBars, RenderMovementStatePanel, via HotbarOrigin below)
// can't drift out of sync with each other the way four independently
// hand-tuned pixel offsets eventually would.
namespace {
    constexpr float SLOT_SIZE = 40.0f;
    constexpr float SLOT_GAP = 4.0f;
    constexpr int HOTBAR_SLOTS = 9;
    constexpr int ARMOR_SLOTS = 4;
    constexpr float HOTBAR_WIDTH = HOTBAR_SLOTS * SLOT_SIZE + (HOTBAR_SLOTS - 1) * SLOT_GAP;
    // Armor is a horizontal row (see RenderHotbarAndArmor) — the "sideways"
    // layout uku's Armor HUD mod uses instead of vanilla's vertical
    // inventory-screen stack — so it shares the hotbar row's own height
    // (SLOT_SIZE) and needs no separate vertical-centering math.
    constexpr float ARMOR_WIDTH = ARMOR_SLOTS * SLOT_SIZE + (ARMOR_SLOTS - 1) * SLOT_GAP;
    constexpr float HUD_BOTTOM_MARGIN = 10.0f; // hotbar's own inset from the bottom edge
    constexpr float PANEL_GAP = 10.0f;         // hotbar <-> armor / hotbar <-> state-panel spacing

    // Health and hunger side by side (not stacked) spanning the hotbar's own
    // width; saturation isn't a third row — vanilla treats it as a "buffer"
    // layered on top of the hunger value rather than its own stat, so it's
    // drawn as a semi-transparent overlay on the hunger bar instead (see
    // DrawStatBar's overlayFrac).
    constexpr float BAR_HEIGHT = 22.0f;
    constexpr float BAR_GAP = 4.0f;
    constexpr float BAR_WIDTH = (HOTBAR_WIDTH - BAR_GAP) * 0.5f;
    constexpr float BAR_PADDING = 8.0f;
    constexpr float STATUS_PANEL_WIDTH = HOTBAR_WIDTH + BAR_PADDING * 2.0f;
    constexpr float STATUS_PANEL_HEIGHT = BAR_HEIGHT + BAR_PADDING * 2.0f;
    constexpr float STATUS_PANEL_GAP = 6.0f;   // status panel <-> hotbar spacing

    // Empty slot rect in the Volcano theme's flat-glow language (same fill/
    // border colors DrawStyledButton uses) — hotbar and armor both need
    // this and nothing else, so it's a free function rather than a member.
    // `selected` swaps the dim border for a bright one plus a soft outer
    // glow (matching DrawStyledButton's own hover glow) — the hotbar's
    // held-slot indicator; nothing else ever passes true for it.
    void DrawSlot(ImDrawList* drawList, ImVec2 pos, float size, bool selected = false)
    {
        constexpr float rounding = 4.0f;
        ImVec2 rectMax(pos.x + size, pos.y + size);

        if (selected)
        {
            drawList->AddRectFilled(
                ImVec2(pos.x - 3.0f, pos.y - 3.0f), ImVec2(rectMax.x + 3.0f, rectMax.y + 3.0f),
                IM_COL32(60, 220, 255, 55), rounding + 3.0f);
        }

        drawList->AddRectFilled(pos, rectMax, IM_COL32(12, 32, 40, 235), rounding);
        drawList->AddRect(pos, rectMax, selected ? IM_COL32(120, 235, 255, 255) : IM_COL32(60, 170, 190, 160),
                           rounding, 0, selected ? 2.5f : 1.5f);
    }

    // Draws one item's icon plus (when > 1) its stack count into an
    // already-drawn slot rect — call after DrawSlot for the same pos/size.
    // `icon` is a GUIController::IconForItem()/GetItemIcon() result; 0
    // (empty slot, unresolved item id, or no icon texture for this item —
    // see IconForItem's own comment) draws nothing, leaving the empty slot
    // rect underneath to read on its own rather than drawing a blank or
    // garbage image.
    void DrawItemIcon(ImDrawList* drawList, ImVec2 pos, float size, ImTextureID icon, uint8_t count)
    {
        if (icon == 0) return;

        constexpr float padding = 4.0f; // Icon inset from the slot's own border, so it doesn't touch/overlap it.
        drawList->AddImage(icon, ImVec2(pos.x + padding, pos.y + padding),
                            ImVec2(pos.x + size - padding, pos.y + size - padding));

        if (count > 1)
        {
            char text[8];
            std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(count));
            ImVec2 textSize = ImGui::CalcTextSize(text);
            // Bottom-right corner, matching vanilla's own stack-count placement.
            ImVec2 textPos(pos.x + size - textSize.x - 3.0f, pos.y + size - textSize.y - 2.0f);
            // A 1px dark "shadow" offset behind the count, same trick
            // vanilla's own font renderer uses, so white text stays legible
            // over a bright icon (snow, a diamond, ...) instead of blending in.
            drawList->AddText(ImVec2(textPos.x + 1.0f, textPos.y + 1.0f), IM_COL32(0, 0, 0, 200), text);
            drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), text);
        }
    }

    // One labeled, proportionally-filled bar — health and hunger both share
    // this, only the fill color and current/max differ. `overlayFrac` (0
    // when unused) draws a second, low-alpha fill on top spanning that
    // fraction of the bar's width — hunger's saturation "buffer" uses this;
    // health doesn't pass one. Values are drawn (not just the bar) since
    // without an icon font there's otherwise no way to read the exact
    // number at a glance. Fill colors are deliberately muted/dark rather
    // than vivid — a first pass used bright red/orange, which made the
    // centered white label text hard to read against them.
    void DrawStatBar(ImDrawList* drawList, ImVec2 pos, float width, float height,
                      const char* label, float current, float max, ImU32 fillColor,
                      float overlayFrac = 0.0f)
    {
        ImVec2 rectMax(pos.x + width, pos.y + height);
        drawList->AddRectFilled(pos, rectMax, IM_COL32(12, 32, 40, 235), 4.0f);

        float frac = max > 0.0f ? std::clamp(current / max, 0.0f, 1.0f) : 0.0f;
        if (frac > 0.0f)
        {
            drawList->AddRectFilled(pos, ImVec2(pos.x + width * frac, rectMax.y), fillColor, 4.0f);
        }

        if (overlayFrac > 0.0f)
        {
            float clamped = std::clamp(overlayFrac, 0.0f, 1.0f);
            drawList->AddRectFilled(pos, ImVec2(pos.x + width * clamped, rectMax.y),
                                     IM_COL32(255, 250, 210, 60), 4.0f);
        }

        drawList->AddRect(pos, rectMax, IM_COL32(60, 170, 190, 160), 4.0f, 0, 1.5f);

        char text[64];
        std::snprintf(text, sizeof(text), "%s  %.1f / %.0f", label, current, max);
        ImVec2 textSize = ImGui::CalcTextSize(text);
        ImVec2 textPos(pos.x + (width - textSize.x) * 0.5f, pos.y + (height - textSize.y) * 0.5f);
        drawList->AddText(textPos, IM_COL32(235, 248, 255, 255), text);
    }
}

// Top-left of the hotbar — see the header comment on why every other HUD
// piece in this group derives its position from this single call rather
// than each re-deriving "bottom center" independently.
ImVec2 GUIController::HotbarOrigin()
{
    return ResolveAnchor(GUI::ScreenAnchor::BOTTOM, ImVec2(HOTBAR_WIDTH, SLOT_SIZE), ImVec2(0.0f, HUD_BOTTOM_MARGIN));
}

// Hotbar (9 slots, held slot highlighted — see selectedSlot below) + armor
// row (4 slots, sideways like uku's Armor HUD — see ARMOR_WIDTH's comment),
// immediately left of it and sharing its row height. Draws real item icons
// (IconForItem/DrawItemIcon) now that GlobalState::inventory is actually
// populated by NetworkClient's Set Container Content/Slot handlers — a slot
// this client has no icon texture for (see IconForItem's own comment) still
// falls back to an empty-looking slot rect rather than nothing/garbage.
// Drawn with raw ImDrawList calls in one NoBackground window rather than
// through GUIWindow/GUIComponent, which has no notion of an icon-slot grid,
// and rather than two separate windows, since a shared draw list means
// there's no z-order question between the two groups of rects.
void GUIController::RenderHotbarAndArmor()
{
    ImVec2 hotbarPos = HotbarOrigin();

    // Read straight from the data model rather than keeping a GUIController-
    // local copy — see InventoryManager::selectedHotbarSlot's own comment.
    // NetworkClient's Set Held Item handler writes here (under
    // inventory.mutex, matching every other cross-thread field in
    // GlobalState) and the HUD picks it up automatically, with no second
    // call site to remember. Hotbar/armor items are copied out under the
    // same lock, rather than held while drawing, so this window's own
    // (potentially slow, first-icon-of-a-frame) Vulkan calls below never
    // run with inventory.mutex held.
    uint8_t selectedSlot;
    std::array<ItemStack, HOTBAR_SLOTS> hotbarItems;
    std::array<ItemStack, ARMOR_SLOTS> armorItems;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        selectedSlot = state->inventory.selectedHotbarSlot;
        for (int i = 0; i < HOTBAR_SLOTS; i++) hotbarItems[static_cast<size_t>(i)] = state->inventory.hotbar.Get(static_cast<size_t>(i));
        for (int i = 0; i < ARMOR_SLOTS; i++) armorItems[static_cast<size_t>(i)] = state->inventory.armor.Get(static_cast<size_t>(i));
    }

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    ImGui::Begin("##hotbar_armor", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    for (int i = 0; i < HOTBAR_SLOTS; i++)
    {
        ImVec2 slotPos(hotbarPos.x + i * (SLOT_SIZE + SLOT_GAP), hotbarPos.y);
        const ItemStack& item = hotbarItems[static_cast<size_t>(i)];
        DrawSlot(drawList, slotPos, SLOT_SIZE, i == selectedSlot);
        DrawItemIcon(drawList, slotPos, SLOT_SIZE, IconForItem(item), item.count);
    }

    ImVec2 armorPos(hotbarPos.x - PANEL_GAP - ARMOR_WIDTH, hotbarPos.y);
    for (int i = 0; i < ARMOR_SLOTS; i++)
    {
        ImVec2 slotPos(armorPos.x + i * (SLOT_SIZE + SLOT_GAP), armorPos.y);
        const ItemStack& item = armorItems[static_cast<size_t>(i)];
        DrawSlot(drawList, slotPos, SLOT_SIZE);
        DrawItemIcon(drawList, slotPos, SLOT_SIZE, IconForItem(item), item.count);
    }

    ImGui::End();
}

// Health and hunger, side by side, directly above the hotbar's horizontal
// span. Vanilla draws two rows of small heart/hunger icons here; there's no
// icon font/texture yet (see Init()'s TODO on merging one into the atlas
// later), so this draws labeled proportional bars instead. Saturation isn't
// a third bar — it's a semi-transparent overlay on top of the hunger bar
// (see DrawStatBar's overlayFrac), matching vanilla's own treatment of it as
// a buffer on the hunger value rather than an independently displayed stat.
// Saturation's real range is 0-20 (it can't exceed the current food level,
// which itself caps at 20) — not the 0-5 a first pass assumed.
void GUIController::RenderPlayerStatusBars()
{
    float health, saturation;
    int32_t food;
    {
        std::lock_guard<std::mutex> lock(state->healthMutex);
        health = state->health;
        food = state->food;
        saturation = state->saturation;
    }

    ImVec2 hotbarPos = HotbarOrigin();
    ImVec2 panelPos(
        hotbarPos.x - BAR_PADDING,
        hotbarPos.y - STATUS_PANEL_GAP - STATUS_PANEL_HEIGHT);

    ImGui::SetNextWindowPos(panelPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(STATUS_PANEL_WIDTH, STATUS_PANEL_HEIGHT), ImGuiCond_Always);
    // NoBackground — each bar already draws its own dark backing rect (see
    // DrawStatBar), so the panel's own window chrome was a second,
    // redundant dark rounded box sitting behind that; removing it lets
    // whatever's behind (world, chat) show through the gap between the two
    // bars instead of a solid panel.
    ImGui::Begin("##status_bars", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float barY = panelPos.y + BAR_PADDING;
    float healthX = panelPos.x + BAR_PADDING;
    float hungerX = healthX + BAR_WIDTH + BAR_GAP;

    DrawStatBar(drawList, ImVec2(healthX, barY), BAR_WIDTH, BAR_HEIGHT,
                "Health", health, 20.0f, IM_COL32(140, 45, 45, 255));
    DrawStatBar(drawList, ImVec2(hungerX, barY), BAR_WIDTH, BAR_HEIGHT,
                "Hunger", static_cast<float>(food), 20.0f, IM_COL32(140, 95, 45, 255),
                saturation / 20.0f);

    ImGui::End();
}

// Current movement state, immediately right of the hotbar and vertically
// centered on it — priority order Flying > Swimming > Sneaking > Sprinting
// > Walking, showing only the highest-priority one that applies (falls
// back to "Idle" when grounded and stationary).
void GUIController::RenderMovementStatePanel()
{
    ImVec2 hotbarPos = HotbarOrigin();
    constexpr float panelWidth = 140.0f;
    ImVec2 panelPos(hotbarPos.x + HOTBAR_WIDTH + PANEL_GAP, hotbarPos.y);

    ImGui::SetNextWindowPos(panelPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelWidth, SLOT_SIZE), ImGuiCond_Always);
    ImGui::Begin("##movement_state", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs);

    // Flying/Swimming are hardcoded false rather than guessed at: this
    // client has no fly toggle (no ability to leave the ground under its
    // own control yet) and no fluid collision (BlockRegistry explicitly
    // excludes fluid blockstates from collision, so there's no "in water"
    // signal to read) — whoever implements either feature flips the
    // matching bool here without touching the priority logic below.
    constexpr bool isFlying = false;
    constexpr bool isSwimming = false;
    bool sneaking = state->input->IsActive("Sneak");
    bool sprinting = state->input->IsActive("Sprint");
    bool hasMoveInput =
        std::fabs(state->input->GetAxis("Move.Forward")) > 0.01f ||
        std::fabs(state->input->GetAxis("Move.Right")) > 0.01f;

    const char* label = "Idle";
    if (isFlying) label = "Flying";
    else if (isSwimming) label = "Swimming";
    else if (sneaking) label = "Sneaking";
    else if (sprinting) label = "Sprinting";
    else if (hasMoveInput) label = "Walking";

    ImVec2 textSize = ImGui::CalcTextSize(label);
    ImGui::SetCursorPos(ImVec2((panelWidth - textSize.x) * 0.5f, (SLOT_SIZE - textSize.y) * 0.5f));
    ImGui::TextColored(ImVec4(0.35f, 0.92f, 1.00f, 1.00f), "%s", label);

    ImGui::End();
}

// See the header's own comment — set as inventoryScreen->customContent, so
// RenderScreen calls this (with its darkened fullscreen "##screen" window
// and title already drawn) whenever the inventory screen is open. Armor +
// offhand as one row (matching the HUD's own "sideways armor" language,
// see ARMOR_WIDTH's comment, rather than switching to vanilla's vertical
// stack here), then the 9x3 main grid, then the hotbar — the grid and
// hotbar share GRID_WIDTH/HOTBAR_WIDTH (both 9 slots wide) so they line up
// the same way vanilla's own inventory screen does.
void GUIController::RenderInventoryScreen()
{
    // Snapshot every slot under one lock rather than holding inventory.mutex
    // while drawing (which includes this frame's Vulkan icon-view/
    // descriptor-set creation on a cache miss) — same reasoning as
    // RenderHotbarAndArmor's own copy.
    std::array<ItemStack, InventoryManager::ARMOR_SIZE> armorItems;
    ItemStack offhandItem;
    std::array<ItemStack, InventoryManager::MAIN_INVENTORY_SIZE> mainItems;
    std::array<ItemStack, HOTBAR_SLOTS> hotbarItems;
    uint8_t selectedSlot;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        for (size_t i = 0; i < armorItems.size(); i++) armorItems[i] = state->inventory.armor.Get(i);
        offhandItem = state->inventory.offhand.Get(0);
        for (size_t i = 0; i < mainItems.size(); i++) mainItems[i] = state->inventory.mainInventory.Get(i);
        for (size_t i = 0; i < hotbarItems.size(); i++) hotbarItems[i] = state->inventory.hotbar.Get(i);
        selectedSlot = state->inventory.selectedHotbarSlot;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    constexpr int MAIN_COLUMNS = 9;
    constexpr int MAIN_ROWS = 3; // InventoryManager::MAIN_INVENTORY_SIZE (27) / MAIN_COLUMNS.
    constexpr float GRID_WIDTH = MAIN_COLUMNS * SLOT_SIZE + (MAIN_COLUMNS - 1) * SLOT_GAP; // Same width as HOTBAR_WIDTH.
    constexpr float ROW_GAP = 8.0f;    // Between the main grid's own rows.
    constexpr float GROUP_GAP = 18.0f; // Armor row <-> main grid, main grid <-> hotbar.
    constexpr float ARMOR_OFFHAND_GAP = 16.0f;
    constexpr float ARMOR_ROW_WIDTH = ARMOR_SLOTS * SLOT_SIZE + (ARMOR_SLOTS - 1) * SLOT_GAP + ARMOR_OFFHAND_GAP + SLOT_SIZE;

    float centerX = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() * 0.5f;
    float contentTop = ImGui::GetCursorScreenPos().y; // Kept so Dummy() below can claim the whole drawn span.
    float y = contentTop;

    float armorX = centerX - ARMOR_ROW_WIDTH * 0.5f;
    for (int i = 0; i < ARMOR_SLOTS; i++)
    {
        ImVec2 slotPos(armorX + i * (SLOT_SIZE + SLOT_GAP), y);
        const ItemStack& item = armorItems[static_cast<size_t>(i)];
        DrawSlot(drawList, slotPos, SLOT_SIZE);
        DrawItemIcon(drawList, slotPos, SLOT_SIZE, IconForItem(item), item.count);
    }
    {
        ImVec2 offhandPos(armorX + ARMOR_SLOTS * (SLOT_SIZE + SLOT_GAP) - SLOT_GAP + ARMOR_OFFHAND_GAP, y);
        DrawSlot(drawList, offhandPos, SLOT_SIZE);
        DrawItemIcon(drawList, offhandPos, SLOT_SIZE, IconForItem(offhandItem), offhandItem.count);
    }

    y += SLOT_SIZE + GROUP_GAP;
    float gridX = centerX - GRID_WIDTH * 0.5f;

    for (int row = 0; row < MAIN_ROWS; row++)
    {
        for (int col = 0; col < MAIN_COLUMNS; col++)
        {
            int index = row * MAIN_COLUMNS + col;
            ImVec2 slotPos(gridX + col * (SLOT_SIZE + SLOT_GAP), y + row * (SLOT_SIZE + ROW_GAP));
            const ItemStack& item = mainItems[static_cast<size_t>(index)];
            DrawSlot(drawList, slotPos, SLOT_SIZE);
            DrawItemIcon(drawList, slotPos, SLOT_SIZE, IconForItem(item), item.count);
        }
    }

    y += MAIN_ROWS * SLOT_SIZE + (MAIN_ROWS - 1) * ROW_GAP + GROUP_GAP;

    for (int i = 0; i < HOTBAR_SLOTS; i++)
    {
        ImVec2 slotPos(gridX + i * (SLOT_SIZE + SLOT_GAP), y);
        const ItemStack& item = hotbarItems[static_cast<size_t>(i)];
        DrawSlot(drawList, slotPos, SLOT_SIZE, i == selectedSlot);
        DrawItemIcon(drawList, slotPos, SLOT_SIZE, IconForItem(item), item.count);
    }

    // Claim the space everything above was just drawn into (with raw
    // screen-space ImDrawList calls, which don't move ImGui's own layout
    // cursor at all) via a real Dummy() item — ImGui hard-asserts if
    // SetCursorScreenPos() alone is used to extend a window's content
    // bounds without submitting something to actually claim that space
    // ("Code uses SetCursorPos()/SetCursorScreenPos() to extend window/
    // parent boundaries..."). The cursor is still sitting at contentTop
    // (nothing above moved it), so Dummy() here — sized to the full drawn
    // span, not just the delta — grows the window correctly in one call.
    ImGui::Dummy(ImVec2(GRID_WIDTH, (y + SLOT_SIZE) - contentTop + 24.0f));
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

// Create a new GUI window, pinned to a screen anchor + offset rather than
// an absolute position — see GUI::ScreenAnchor and ResolveAnchor().
GUI::GUIWindow* GUIController::CreateWindow(const std::string& name, GUI::ScreenAnchor anchor,
                                             float offsetX, float offsetY)
{
    constexpr uint16_t width = 180, height = 80;

    GUI::GUIWindow* window = new GUI::GUIWindow{name};
    window->components = {};
    window->anchor = anchor;
    window->offsetX = offsetX;
    window->offsetY = offsetY;
    window->sizeX = width;
    window->sizeY = height;

    windows.push_back(window);
    return window;
}

// Resolves anchor + inward pixel offset into an absolute top-left position
// against the current ImGui display size — see the header comment.
ImVec2 GUIController::ResolveAnchor(GUI::ScreenAnchor anchor, ImVec2 size, ImVec2 offset)
{
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float x = 0.0f, y = 0.0f;

    switch (anchor)
    {
        default:
        case GUI::ScreenAnchor::TOP_LEFT:
        case GUI::ScreenAnchor::LEFT:
        case GUI::ScreenAnchor::BOTTOM_LEFT:
            x = offset.x;
            break;
        case GUI::ScreenAnchor::TOP_RIGHT:
        case GUI::ScreenAnchor::RIGHT:
        case GUI::ScreenAnchor::BOTTOM_RIGHT:
            x = display.x - size.x - offset.x;
            break;
        case GUI::ScreenAnchor::TOP:
        case GUI::ScreenAnchor::BOTTOM:
        case GUI::ScreenAnchor::CENTER:
            x = (display.x - size.x) * 0.5f + offset.x;
            break;
    }

    switch (anchor)
    {
        default:
        case GUI::ScreenAnchor::TOP_LEFT:
        case GUI::ScreenAnchor::TOP:
        case GUI::ScreenAnchor::TOP_RIGHT:
            y = offset.y;
            break;
        case GUI::ScreenAnchor::BOTTOM_LEFT:
        case GUI::ScreenAnchor::BOTTOM:
        case GUI::ScreenAnchor::BOTTOM_RIGHT:
            y = display.y - size.y - offset.y;
            break;
        case GUI::ScreenAnchor::LEFT:
        case GUI::ScreenAnchor::RIGHT:
        case GUI::ScreenAnchor::CENTER:
            y = (display.y - size.y) * 0.5f + offset.y;
            break;
    }

    return ImVec2(x, y);
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
// so ImGui (rather than the fly-cam) drives it. GUIController runs on the
// render thread, but GLFW only guarantees glfwSetInputMode is safe from the
// main (window-owning) thread — see GlobalState::pendingCursorMode, applied
// by VolcanoClient.cpp's main loop.
void GUIController::OpenScreen(GUI::Screen* screen)
{
    activeScreen = screen;
    if (windowHandle != nullptr)
    {
        state->pendingCursorMode.store(GLFW_CURSOR_NORMAL);
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
        if (state->windowFocused.load())
        {
            state->pendingCursorMode.store(GLFW_CURSOR_DISABLED);
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
        state->pendingCursorMode.store(GLFW_CURSOR_NORMAL);
    }
}

// Close the chat input box and re-capture the cursor for the fly-cam —
// same focus caveat CloseScreen's own comment explains (only re-lock if
// the window is actually focused right now).
void GUIController::CloseChatInput()
{
    chatInputOpen = false;
    chatInputJustOpened = false;
    if (windowHandle != nullptr && state->windowFocused.load())
    {
        state->pendingCursorMode.store(GLFW_CURSOR_DISABLED);
    }
}

} // namespace Volcano
