#include "GUIController.hpp"
#include "Logger.hpp"
#include "audio/SoundRegistry.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <imgui.h>
#include <imgui_internal.h>
#include <cfloat>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>
#include <cstring>
#include <unordered_set>
#include <stb_image.h>
#include "gui/models/GUIComponent.hpp"
#include "gui/models/Button.hpp"
#include "gui/models/Chat.hpp"
#include "gui/models/NumberInput.hpp"
#include "gui/models/OptionButton.hpp"
#include "gui/models/Slider.hpp"
#include "inventory/ItemRegistry.hpp"
#include "TickLoop.hpp"
#include "SystemInfo.hpp"
#include "renderer/engine/SceneView.hpp"
#include "renderer/entity/models/EntityRegistry.hpp"
#include "platform/OS.hpp"
#include "renderer/VulkanInit.hpp"
#include "renderer/terrain/ChunkMesher.hpp"
#include "renderer/terrain/OcclusionSnapshot.hpp"
#include "renderer/misc/NonCubicMesher.hpp"
#include "renderer/misc/ItemModel.hpp"
#include "renderer/misc/ItemRenderer.hpp"

namespace Volcano {

// Pixel-art GUI textures are drawn with nearest sampling so scaled-up art
// stays crisp; text and 3D icons keep ImGui's linear sampler.
static std::unordered_set<ImTextureID> linearIconTextures;

static void AddImageNearest(ImDrawList* drawList, ImTextureID texture, const ImVec2& min, const ImVec2& max,
    const ImVec2& uv0 = ImVec2(0.0f, 0.0f), const ImVec2& uv1 = ImVec2(1.0f, 1.0f), ImU32 color = IM_COL32_WHITE)
{
    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    drawList->AddCallback(platformIO.DrawCallback_SetSamplerNearest, nullptr);
    drawList->AddImage(texture, min, max, uv0, uv1, color);
    drawList->AddCallback(platformIO.DrawCallback_SetSamplerLinear, nullptr);
}

// Declared here rather than including network/NetworkClient.hpp: that
// header drags in asio.hpp, which on Windows pulls in <windows.h> —
// whose CreateWindowA/CreateWindow macros collide with
// GUIController::CreateWindow below. This one free function is all
// GUIController actually needs from it.
void SendChatMessage(GlobalState* state, const std::string& message);
void SendChatCommand(GlobalState* state, const std::string& command);
void SendRespawnRequest(GlobalState* state);
void SendInventoryClick(GlobalState* state, int32_t windowId, int32_t stateId, int32_t slot,
                        uint8_t button, const ItemStack& slotItem, const ItemStack& carriedItem);
void SendInventoryClose(GlobalState* state, int32_t windowId);
}

namespace Volcano {

namespace {
    // Vanilla art is drawn at (Graphics.GUIScale + 1) device pixels per
    // texture pixel, since the rest of this GUI uses bigger numbers than
    // vanilla's. ImGui units are already divided by the GUI scale, so the
    // extra +1 is a (scale + 1) / scale factor on vanilla-sized geometry.
    float g_artScale = 1.0f;
    float Art(float units) { return units * g_artScale; }
}

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
std::unordered_map<uint16_t, GUIController::ModelIcon> GUIController::modelIconCache;
GUIController::StandaloneIcon GUIController::crosshairIcon;
GUIController::StandaloneIcon GUIController::crosshairEntityIcon;
GUIController::StandaloneIcon GUIController::attackIndicatorBackgroundIcon;
GUIController::StandaloneIcon GUIController::attackIndicatorProgressIcon;
GUIController::StandaloneIcon GUIController::hotbarIcon;
GUIController::StandaloneIcon GUIController::hotbarSelectionIcon;
GUIController::StandaloneIcon GUIController::slotHighlightBackIcon;
GUIController::StandaloneIcon GUIController::slotHighlightFrontIcon;
GUIController::StandaloneIcon GUIController::inventoryBackground;
GUIController::StandaloneIcon GUIController::craftingBackground;
GUIController::StandaloneIcon GUIController::furnaceBackground;
GUIController::StandaloneIcon GUIController::genericBackground;
GUIController::StandaloneIcon GUIController::logoIcon;

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
    fontConfig.OversampleH = 1; // This is a pixel font, so oversampling looks weird.
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true; // whole-pixel glyph positions, also for the pixel font
    io.Fonts->AddFontFromFileTTF("resources/fonts/Minecraft.ttf", 16.0f, &fontConfig);

    // System fonts merged in as fallbacks for characters Minecraft.ttf lacks
    // (box drawing, symbols, emoji in chat); glyphs load on demand.
    for (const std::string& path : Platform::OS::GetFallbackFontPaths())
    {
        ImFontConfig fallbackConfig;
        fallbackConfig.MergeMode = true;
        fallbackConfig.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(path.c_str(), 16.0f, &fallbackConfig);
    }

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

    const std::string guiTextures = "resources/minecraft/assets/minecraft/textures/gui/";
    attackIndicatorBackgroundIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/crosshair_attack_indicator_background.png");
    attackIndicatorProgressIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/crosshair_attack_indicator_progress.png");
    hotbarIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/hotbar.png");
    hotbarSelectionIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/hotbar_selection.png");
    slotHighlightBackIcon = LoadStandaloneIcon(guiTextures + "sprites/container/slot_highlight_back.png");
    slotHighlightFrontIcon = LoadStandaloneIcon(guiTextures + "sprites/container/slot_highlight_front.png");
    inventoryBackground = LoadStandaloneIcon(guiTextures + "container/inventory.png");
    craftingBackground = LoadStandaloneIcon(guiTextures + "container/crafting_table.png");
    furnaceBackground = LoadStandaloneIcon(guiTextures + "container/furnace.png");
    genericBackground = LoadStandaloneIcon(guiTextures + "container/generic_54.png");
    logoIcon = LoadStandaloneIcon("resources/logo.png");

    Log::Info("GUI Controller initialized with ImGui");
    initialized = true;
}

void GUIController::Shutdown()
{
    if (!initialized) return;

    ImGuiVulkan::Shutdown(imguiContext);

    for (auto& [layer, icon] : itemIconCache) {
        if (icon.view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), icon.view, nullptr);
    }
    itemIconCache.clear();

    for (auto& [itemId, entry] : modelIconCache) {
        if (entry.icon.view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), entry.icon.view, nullptr);
        if (entry.icon.image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, entry.icon.image, entry.icon.allocation);
    }
    modelIconCache.clear();

    // Unlike itemIconCache's views above, these own the whole image (see
    // StandaloneIcon's own comment) — the view alone isn't enough to free.
    for (StandaloneIcon* icon : { &crosshairIcon, &crosshairEntityIcon, &attackIndicatorBackgroundIcon,
            &attackIndicatorProgressIcon, &hotbarIcon, &hotbarSelectionIcon,
            &slotHighlightBackIcon, &slotHighlightFrontIcon, &inventoryBackground, &craftingBackground,
            &furnaceBackground, &genericBackground, &logoIcon }) {
        if (icon->view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), icon->view, nullptr);
        if (icon->image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, icon->image, icon->allocation);
        *icon = StandaloneIcon{};
    }

    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    Log::Info("GUI Controller shutdown");
    initialized = false;
}

ImTextureID GUIController::TextureOf(const StandaloneIcon& icon)
{
    if (icon.descriptorSet == VK_NULL_HANDLE) return 0;
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(icon.descriptorSet));
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
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM; // Matches TextureManager::CreateArrayImage's own format.
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1; // Icons are drawn small; no need for the world pass's full mip chain.
    viewInfo.subresourceRange.baseArrayLayer = textureLayer;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &icon.view) != VK_SUCCESS) {
        Log::Error("GUIController: failed to create an item icon view for texture layer "
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
    if (!info) return 0;

    // Blocks and custom models draw as 3D icons; flat items keep their crisp
    // 2D sprite.
    if (ImTextureID icon = ModelIconForItem(item.itemId, info->name)) return icon;
    if (!info->hasTexture) return 0;
    return GetItemIcon(info->textureLayer);
}

ImTextureID GUIController::ModelIconForItem(uint16_t itemId, const std::string& itemName)
{
    ModelIcon& entry = modelIconCache[itemId];
    if (!entry.resolved)
    {
        entry.resolved = true;
        if (state != nullptr && state->itemRenderer != nullptr && textureManager != nullptr)
        {
            const ItemModel* model = ItemModel::ForItem(itemName, *textureManager);
            if (model != nullptr && model->GetType() != ItemModelType::ITEM)
            {
                ItemRenderer::IconImage image;
                if (state->itemRenderer->RenderGuiIcon(itemName, image))
                {
                    entry.icon.image = image.image;
                    entry.icon.allocation = image.allocation;
                    entry.icon.view = image.view;
                    entry.icon.descriptorSet = ImGui_ImplVulkan_AddTexture(image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    linearIconTextures.insert(TextureOf(entry.icon));
                }
            }
        }
    }
    return TextureOf(entry.icon);
}

// See the header's own comment.
GUIController::StandaloneIcon GUIController::LoadStandaloneIcon(const std::string& path)
{
    StandaloneIcon icon;

    int width = 0, height = 0, channels = 0;
    uint8_t* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        Log::Error("GUIController: failed to load " + path + ", skipping.");
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
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(vmaAllocator, &imageInfo, &allocInfo, &icon.image, &icon.allocation, nullptr) != VK_SUCCESS) {
        Log::Error("GUIController: failed to create image for " + path);
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
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &icon.view) != VK_SUCCESS) {
        Log::Error("GUIController: failed to create image view for " + path);
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

    AddImageNearest(ImGui::GetForegroundDrawList(), texture,
        ImVec2(center.x - half.x, center.y - half.y),
        ImVec2(center.x + half.x, center.y + half.y));

    RenderAttackIndicator(center);
}

void GUIController::RenderAttackIndicator(const ImVec2& center)
{
    float current = state->crosshairProgressCurrent.load(std::memory_order_relaxed);
    if (current < 0.0f) return; // Nothing mining or recharging.

    ImTextureID background = TextureOf(attackIndicatorBackgroundIcon);
    ImTextureID progress = TextureOf(attackIndicatorProgressIcon);
    if (background == 0 || progress == 0) return;

    // Lerp last tick's value to this tick's by the elapsed tick fraction.
    float previous = state->crosshairProgressPrevious.load(std::memory_order_relaxed);
    float value = previous + (current - previous) * state->tickLoop->GetPartialTick();

    // Vanilla fills (int)(value * 17) of the 16 texture pixels; kept
    // fractional here so the interpolation stays smooth.
    float fillPixels = std::clamp(value * 17.0f, 0.0f, 16.0f);

    ImVec2 min(center.x - Art(8.0f), center.y + Art(16.0f));
    ImVec2 max(min.x + Art(16.0f), min.y + Art(4.0f));
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    AddImageNearest(drawList, background, min, max);

    if (fillPixels <= 0.0f) return;
    AddImageNearest(drawList, progress, min, ImVec2(min.x + Art(fillPixels), max.y),
        ImVec2(0.0f, 0.0f), ImVec2(fillPixels / 16.0f, 1.0f), IM_COL32_WHITE);
}

void GUIController::NewFrame()
{
    if (!initialized) return;

    ImGui_ImplGlfw_NewFrame();
    ImGuiVulkan::NewFrame(imguiContext);
    ApplyGUIScale();
    ImGui::NewFrame();

    // Events left queued for a later frame (input trickling) are already
    // scaled; ApplyGUIScale only touches newer ones.
    scaledInputEvents = ImGui::GetCurrentContext()->InputEventsQueue.Size;
}

// Graphics.GUIScale: 0 = Auto, else an integer multiplier. The UI is laid
// out at DisplaySize / scale and drawn at scale x density (fonts are
// rasterized at that density too, so text stays crisp). Mouse positions
// arrive in window pixels and are scaled down to match.
void GUIController::ApplyGUIScale()
{
    ImGuiIO& io = ImGui::GetIO();
    uint32_t setting = std::get<uint32_t>(state->config->Get("Graphics.GUIScale", uint32_t{0}));

    // Auto: the largest scale that keeps at least a 640x360 layout.
    float scale = static_cast<float>(setting);
    if (setting == 0)
    {
        scale = std::floor(std::min(io.DisplaySize.x / 640.0f, io.DisplaySize.y / 360.0f));
    }
    scale = std::max(1.0f, scale);
    guiScale = scale;
    g_artScale = (scale + 1.0f) / scale;

    io.DisplaySize = ImVec2(io.DisplaySize.x / scale, io.DisplaySize.y / scale);
    io.DisplayFramebufferScale = ImVec2(io.DisplayFramebufferScale.x * scale, io.DisplayFramebufferScale.y * scale);

    ImVector<ImGuiInputEvent>& events = ImGui::GetCurrentContext()->InputEventsQueue;
    for (int i = std::min(scaledInputEvents, events.Size); i < events.Size; i++)
    {
        if (events[i].Type != ImGuiInputEventType_MousePos) continue;
        if (events[i].MousePos.PosX == -FLT_MAX) continue; // "mouse unavailable" marker
        events[i].MousePos.PosX /= scale;
        events[i].MousePos.PosY /= scale;
    }
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

    bool openServerWindow = false;
    bool closeServerWindow = false;
    std::string serverWindowTitle;
    int32_t serverWindowType = 0;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        openServerWindow = state->inventory.openWindowPending;
        closeServerWindow = state->inventory.closeWindowPending;
        serverWindowTitle = state->inventory.activeWindowTitle;
        serverWindowType = state->inventory.activeWindowType;
        state->inventory.openWindowPending = false;
        state->inventory.closeWindowPending = false;
    }
    if (closeServerWindow && activeScreen == inventoryScreen) CloseScreen();
    if (openServerWindow)
    {
        if (serverWindowType == InventoryManager::CRAFTING_MENU_TYPE) inventoryScreen->title = "Crafting";
        else if (serverWindowType == InventoryManager::FURNACE_MENU_TYPE) inventoryScreen->title = "Furnace";
        else inventoryScreen->title = serverWindowTitle.empty() ? "Container" : serverWindowTitle;
        OpenScreen(inventoryScreen);
    }

    // Esc closes whatever screen is open, same as Minecraft's menus.
    if (activeScreen != nullptr && activeScreen->closable && state->input->IsKeyPressed(GLFW_KEY_ESCAPE))
    {
        CloseScreen();
    }
    // Esc with nothing else open pauses the game instead, same key Minecraft
    // uses for both. Doesn't fire on top of a non-closable screen (e.g. the
    // connect screen) since that leaves activeScreen non-null and the first
    // branch above already owns Esc in that case. Only while actually in a
    // world (ConnectionPhase::Playing) and once a pause screen has been
    // registered (SetPauseScreen).
    else if (activeScreen == nullptr && !chatInputOpen && pauseScreen != nullptr
        && state->connectionPhase.load() == ConnectionPhase::Playing
        && state->input->IsKeyPressed(GLFW_KEY_ESCAPE))
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

            auto ToMB = [](uint64_t bytes) { return std::to_string(bytes / (1024ull * 1024ull)) + " MB"; };
            auto FormatMs = [](const std::atomic<float>& ms)
            {
                char text[24];
                std::snprintf(text, sizeof(text), "%.2f ms", ms.load());
                return std::string(text);
            };

            // Working set, not committed/virtual size — the number Task
            // Manager's "Memory" column shows. See GetProcessMemoryUsageBytes'
            // own comment for why this lives in SystemInfo.cpp, not here.
            uint64_t ramUsage = GetProcessMemoryUsageBytes();

            VmaBudget heapBudgets[VK_MAX_MEMORY_HEAPS]{};
            vmaGetHeapBudgets(vmaAllocator, heapBudgets);
            uint64_t vramUsage = 0;
            for (const VmaBudget& budget : heapBudgets) vramUsage += budget.usage;

            SlabStats cubicSlab = ChunkMesher::GetSlabStats();
            SlabStats nonCubicSlab = NonCubicMesher::GetSlabStats();

            char buf[96];
            std::snprintf(buf, sizeof(buf), "(%.5f, %.5f, %.5f)", position.x, position.y, position.z);
            std::string posString(buf);

            // What NetworkClient last stored from Update Attributes (or the default).
            double moveSpeed = state->attributes ? state->attributes->GetDouble("generic.movement_speed", -1.0) : -1.0;
            std::snprintf(buf, sizeof(buf), "%.7g", moveSpeed);
            std::string moveSpeedString(buf);

            debugText->SetLabel(
                "\nFPS: " + std::to_string(static_cast<int>(std::round(fps))) +
                "\nFrame Time: " + std::to_string(frameTime) + "ms" +
                "\nTriangles: " + std::to_string(state->trianglesDrawn) +
                "\nRender CPU: " + FormatMs(state->timings.renderCpuMs) +
                "\nWaiting on GPU: " + FormatMs(state->timings.gpuWaitMs) +
                "\nGPU Time: " + (state->timings.gpuMs.load() < 0.0f ? std::string("n/a") : FormatMs(state->timings.gpuMs)) +
                "\nMesher: " + FormatMs(state->timings.meshMs) + "/section, "
                    + std::to_string(state->timings.meshQueue.load()) + " queued" +
                "\nPresent Mode: " + vsync +
                "\nCPU: " + std::to_string(sys.cpuLogicalCores) + "x " + sys.cpuModel +
                "\nRAM: " + ToMB(ramUsage) +
                "\nGPU: " + sys.gpuModel +
                "\nVRAM: " + ToMB(vramUsage) +
                "\nCubic Slab: " + ToMB(cubicSlab.used) + " / " + ToMB(cubicSlab.capacity) +
                "\nNon-Cubic Slab: " + ToMB(nonCubicSlab.used) + " / " + ToMB(nonCubicSlab.capacity) +
                "\nDisplay: " + std::to_string(windowWidth) + "x" + std::to_string(windowHeight)
                    + " (" + sys.gpuDriverVendor + ")" +
                "\nGraphics API: " + vulkanVersion +
                "\nPosition: " + posString +
                "\nmovement_speed: " + moveSpeedString);
        }
    }
}

void GUIController::DiscardFrame()
{
    if (!initialized) return;
    ImGui::EndFrame();
}

void GUIController::RenderNametags(const Engine::SceneView& scene)
{
    constexpr float MAX_DISTANCE = 100.0f;
    constexpr float MAX_DISTANCE_SQUARED = MAX_DISTANCE * MAX_DISTANCE;
    constexpr float HEAD_MARGIN = 0.3f; // Blocks above the bounding box.
    constexpr float SCREEN_MARGIN = 1.3f; // NDC extent kept, so wide tags near an edge still draw.
    constexpr uint64_t PRUNE_INTERVAL = 120;
    constexpr size_t MAX_TAGS = 24; // Nearest only; every tag is two lines of glyph quads plus shadows.
    constexpr float DETAIL_DISTANCE_SQUARED = 24.0f * 24.0f; // The coordinate line is dropped beyond this.

    // Text and its measured width per entity; rebuilt only when the
    // reported position (or the font size) changes, not every frame.
    struct TagText
    {
        glm::vec3 shownPosition{0.0f};
        bool built = false;
        bool detailed = false;
        decltype(Entity::type) type{};
        float fontSize = 0.0f;
        std::string lines[2];
        float widths[2]{};
        uint64_t lastUsed = 0;
    };
    struct Candidate
    {
        float distanceSquared;
        const Entity* entity;
        const std::string* name;
        ImVec2 screen;
    };
    struct Visible
    {
        ImVec2 screen;
        const TagText* text;
        int lineCount;
    };
    static std::unordered_map<uint32_t, TagText> cache;
    static std::vector<Candidate> candidates;
    static std::vector<Visible> visible;
    static uint64_t frame = 0;
    frame++;
    candidates.clear();
    visible.clear();

    glm::mat4 viewProj = scene.proj * scene.view;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float fontSize = ImGui::GetFontSize();
    auto now = std::chrono::steady_clock::now();
    int32_t localId = state->localEntityId.load();

    // Cheap tests only; text is built afterward for the nearest few.
    auto consider = [&](const Entity& entity, const std::string* name)
    {
        if (static_cast<int32_t>(entity.id) == localId || !entity.visible) return;

        glm::vec3 offset = entity.position - scene.cameraPosition;
        float distanceSquared = glm::dot(offset, offset);
        if (distanceSquared > MAX_DISTANCE_SQUARED) return;

        glm::vec3 feet = entity.InterpolatedPosition(now);
        glm::vec4 clip = viewProj * glm::vec4(feet + glm::vec3(0.0f, entity.boundingBox.y + HEAD_MARGIN, 0.0f), 1.0f);
        if (clip.w <= 0.0f) return; // Behind the camera.

        // The projection already carries Vulkan's flipped Y, so NDC y grows downward.
        float ndcX = clip.x / clip.w;
        float ndcY = clip.y / clip.w;
        if (std::abs(ndcX) > SCREEN_MARGIN || std::abs(ndcY) > SCREEN_MARGIN) return;

        // Hi-Z: tags of non-players behind terrain are skipped, like the models.
        if (name == nullptr && occlusionSnapshot.IsEntityOccluded(feet, entity.boundingBox.x, entity.boundingBox.y)) return;

        candidates.push_back({ distanceSquared, &entity, name,
            ImVec2((ndcX * 0.5f + 0.5f) * display.x, (ndcY * 0.5f + 0.5f) * display.y) });
    };

    {
        std::lock_guard<std::mutex> lock(state->entitiesMutex);
        for (const auto& [id, entity] : state->entities) consider(entity, nullptr);
        for (const auto& [id, player] : state->playerEntities) consider(player, &player.username);

        if (candidates.size() > MAX_TAGS)
        {
            std::nth_element(candidates.begin(), candidates.begin() + MAX_TAGS, candidates.end(),
                [](const Candidate& a, const Candidate& b) { return a.distanceSquared < b.distanceSquared; });
            candidates.resize(MAX_TAGS);
        }

        for (const Candidate& candidate : candidates)
        {
            const Entity& entity = *candidate.entity;
            bool detailed = candidate.distanceSquared <= DETAIL_DISTANCE_SQUARED;

            TagText& text = cache[entity.id];
            text.lastUsed = frame;
            if (!text.built || text.type != entity.type || text.detailed != detailed
                || (detailed && text.shownPosition != entity.position) || text.fontSize != fontSize)
            {
                bool typeChanged = !text.built || text.type != entity.type;
                text.built = true;
                text.detailed = detailed;
                text.shownPosition = entity.position;
                text.fontSize = fontSize;
                text.type = entity.type;

                if (typeChanged)
                {
                    const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity.type);
                    text.lines[0] = info != nullptr ? info->name : "unknown";
                    if (candidate.name != nullptr && !candidate.name->empty()) text.lines[0] += " '" + *candidate.name + "'";
                    text.widths[0] = ImGui::CalcTextSize(text.lines[0].c_str()).x;
                }

                if (detailed)
                {
                    char buffer[96];
                    int length = std::snprintf(buffer, sizeof(buffer), "%.2f, %.2f, %.2f", entity.position.x, entity.position.y, entity.position.z);
                    text.lines[1].assign(buffer, static_cast<size_t>(std::max(length, 0)));
                    text.widths[1] = ImGui::CalcTextSize(text.lines[1].c_str()).x;
                }
            }

            visible.push_back({ candidate.screen, &text, detailed ? 2 : 1 });
        }
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    for (const Visible& tag : visible)
    {
        for (int i = 0; i < tag.lineCount; i++)
        {
            const std::string& line = tag.text->lines[i];
            float lineY = tag.screen.y - fontSize * static_cast<float>(tag.lineCount - i);
            ImVec2 position(std::floor(tag.screen.x - tag.text->widths[i] * 0.5f), std::floor(lineY));
            drawList->AddText(nullptr, 0.0f, position, IM_COL32_WHITE, line.data(), line.data() + line.size());
        }
    }

    if (frame % PRUNE_INTERVAL == 0)
    {
        for (auto it = cache.begin(); it != cache.end();)
        {
            it = frame - it->second.lastUsed > PRUNE_INTERVAL ? cache.erase(it) : std::next(it);
        }
    }
}

void GUIController::Render(VkCommandBuffer commandBuffer, const Engine::SceneView* scene)
{
    if (!initialized) return;

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
                size = ImVec2(natural.x, natural.y);
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

        if (scene != nullptr && state->connectionPhase.load() == ConnectionPhase::Playing
            && std::get<bool>(state->config->Get("Graphics.Nametags", true)))
        {
            RenderNametags(*scene);
        }
    }

    ImGui::Render();
    AddTextShadows(ImGui::GetDrawData());
    ImGuiVulkan::RenderDrawData(imguiContext, commandBuffer);
}

// Vanilla-style text shadow on all UI text: a black copy 2 px down-right,
// drawn right before the text it belongs to. Text is identified as font
// atlas triangles not sampling the atlas' white pixel (solid shapes do);
// anti-aliased lines are kept off the atlas (see ApplyVolcanoTheme).
void GUIController::AddTextShadows(ImDrawData* drawData)
{
    constexpr float offset = 2.0f;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    ImVec2 white = atlas->TexUvWhitePixel;
    auto isText = [&](const ImDrawVert& vertex) { return vertex.uv.x != white.x || vertex.uv.y != white.y; };

    for (ImDrawList* list : drawData->CmdLists)
    {
        bool hasText = false;
        for (const ImDrawCmd& cmd : list->CmdBuffer)
        {
            if (cmd.UserCallback == nullptr && cmd.TexRef._TexData == atlas->TexData)
            {
                hasText = true;
                break;
            }
        }
        if (!hasText) continue;

        // Room for a shadow copy of every vertex, so nothing reallocates below.
        int vertexCount = list->VtxBuffer.Size;
        list->VtxBuffer.reserve(vertexCount * 2);
        ImDrawVert* vertices = list->VtxBuffer.Data;
        int vertexCapacity = list->VtxBuffer.Capacity;

        ImVector<ImDrawIdx> newIndices;
        newIndices.reserve(list->IdxBuffer.Size * 2);
        ImDrawIdx* out = newIndices.Data;

        for (ImDrawCmd& cmd : list->CmdBuffer)
        {
            unsigned int newOffset = static_cast<unsigned int>(out - newIndices.Data);
            const ImDrawIdx* indices = list->IdxBuffer.Data + cmd.IdxOffset;
            const ImDrawVert* cmdVertices = vertices + cmd.VtxOffset;
            bool fontCmd = cmd.UserCallback == nullptr && cmd.TexRef._TexData == atlas->TexData;

            unsigned int i = 0;
            while (i < cmd.ElemCount)
            {
                // A run of consecutive text triangles: shadows first, then the text.
                unsigned int runEnd = i;
                while (fontCmd && runEnd + 3 <= cmd.ElemCount
                    && isText(cmdVertices[indices[runEnd]])
                    && isText(cmdVertices[indices[runEnd + 1]])
                    && isText(cmdVertices[indices[runEnd + 2]]))
                {
                    runEnd += 3;
                }

                if (runEnd == i)
                {
                    *out++ = indices[i++];
                    continue;
                }

                // The run's vertices are contiguous; shadow them as one block.
                ImDrawIdx first = indices[i];
                ImDrawIdx last = first;
                for (unsigned int k = i + 1; k < runEnd; k++)
                {
                    first = std::min(first, indices[k]);
                    last = std::max(last, indices[k]);
                }
                int blockSize = static_cast<int>(last - first) + 1;

                if (vertexCount + blockSize <= vertexCapacity)
                {
                    ImDrawIdx shadowBase = static_cast<ImDrawIdx>(vertexCount - static_cast<int>(cmd.VtxOffset));
                    for (int v = 0; v < blockSize; v++)
                    {
                        ImDrawVert shadow = cmdVertices[first + v];
                        shadow.pos.x += offset;
                        shadow.pos.y += offset;
                        shadow.col &= IM_COL32_A_MASK; // black, same alpha as the text
                        vertices[vertexCount++] = shadow;
                    }
                    for (unsigned int k = i; k < runEnd; k++) *out++ = shadowBase + (indices[k] - first);
                }
                for (unsigned int k = i; k < runEnd; k++) *out++ = indices[k];
                i = runEnd;
            }

            cmd.IdxOffset = newOffset;
            cmd.ElemCount = static_cast<unsigned int>(out - newIndices.Data) - newOffset;
        }

        newIndices.Size = static_cast<int>(out - newIndices.Data);
        list->VtxBuffer.Size = vertexCount;
        list->IdxBuffer.swap(newIndices);
    }

    drawData->TotalVtxCount = 0;
    drawData->TotalIdxCount = 0;
    for (ImDrawList* list : drawData->CmdLists)
    {
        drawData->TotalVtxCount += list->VtxBuffer.Size;
        drawData->TotalIdxCount += list->IdxBuffer.Size;
    }
}

// A tooltip beside [itemMin, itemMax] rather than under the cursor, so it
// never covers neighboring settings. Clamped to the screen.
void GUIController::DrawSideTooltip(const std::string& text, ImVec2 itemMin, ImVec2 itemMax, bool onLeft)
{
    constexpr float gap = 12.0f;
    constexpr float maxWidth = 220.0f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 padding = ImGui::GetStyle().WindowPadding;

    // Measure first so the side and clamping are decided before drawing.
    float wrapWidth = maxWidth - padding.x * 2.0f;
    ImVec2 textSize = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapWidth);
    ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);

    // Preferred side, or the other one if it doesn't fit there.
    float roomLeft = itemMin.x - gap;
    float roomRight = display.x - itemMax.x - gap;
    if (onLeft && roomLeft < size.x && roomRight > roomLeft) onLeft = false;
    else if (!onLeft && roomRight < size.x && roomLeft > roomRight) onLeft = true;

    float x = onLeft ? itemMin.x - gap - size.x : itemMax.x + gap;
    float y = (itemMin.y + itemMax.y - size.y) * 0.5f;
    x = std::clamp(x, 0.0f, std::max(0.0f, display.x - size.x));
    y = std::clamp(y, 0.0f, std::max(0.0f, display.y - size.y));

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin("##settingTooltip", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
        | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::End();
}

bool GUIController::IsLabeledControl(GUI::GUIComponent* component)
{
    GUI::GUIComponentType type = component->GetType();
    return type == GUI::GUIComponentType::SLIDER || type == GUI::GUIComponentType::NUMBER
        || type == GUI::GUIComponentType::OPTION;
}

// Render a single component the same way regardless of whether it lives in
// a HUD window or a fullscreen Screen. With labelWidth > 0, labeled controls
// draw their label on the left and only the value in the control itself.
void GUIController::RenderComponent(GUI::GUIComponent* component, float width, float labelWidth)
{
    if (component == nullptr || !component->IsVisible()) return;

    // Taller frames to match DrawStyledButton's height.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y + 4.0f));

    bool labeled = labelWidth > 0.0f && IsLabeledControl(component);
    float controlWidth = width;
    if (labeled)
    {
        float rowX = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(component->GetLabel().c_str());
        ImGui::SameLine(rowX + labelWidth);
        controlWidth = width - labelWidth;
    }
    std::string id = "##" + component->GetLabel();

    switch (component->GetType())
    {
        default:
        case GUI::GUIComponentType::TEXT: {
            ImGui::Text("%s", component->GetLabel().c_str());
            break;
        }
        case GUI::GUIComponentType::BUTTON: {
            if (DrawStyledButton(component->GetLabel(), ImVec2(width, ImGui::GetFrameHeight())))
            {
                state->soundRegistry->Play(state->audioManager, "minecraft:random/click_stereo");
                component->Click();
            }
            break;
        }
        case GUI::GUIComponentType::INPUT: {
            char* buffer = component->GetTextBuffer();
            if (buffer != nullptr)
            {
                ImGui::SetNextItemWidth(width);
                ImGui::InputText(component->GetLabel().c_str(), buffer, component->GetTextBufferCapacity());
            }
            break;
        }
        case GUI::GUIComponentType::SLIDER: {
            auto* slider = static_cast<GUI::Slider*>(component);
            if (!slider->editing) slider->editValue = slider->GetValue();
            // printf-format text: escape '%' so "50%" survives.
            auto escape = [](std::string text)
            {
                for (size_t i = text.find('%'); i != std::string::npos; i = text.find('%', i + 2)) text.insert(i, "%");
                return text;
            };
            std::string format = (labeled ? "" : escape(slider->GetLabel()) + ": ") + "%d" + escape(slider->GetSuffix());
            ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 6.0f); // thin grab, never covers the text
            ImGui::SetNextItemWidth(controlWidth);
            ImGui::SliderInt(id.c_str(), &slider->editValue, slider->GetMin(), slider->GetMax(), format.c_str());
            ImGui::PopStyleVar();
            slider->editing = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                state->soundRegistry->Play(state->audioManager, "minecraft:random/click_stereo");
                slider->SetValue(slider->editValue);
            }
            break;
        }
        case GUI::GUIComponentType::NUMBER: {
            auto* input = static_cast<GUI::NumberInput*>(component);
            if (!input->editing) input->editValue = input->GetValue();
            ImGui::SetNextItemWidth(controlWidth);
            ImGui::InputInt(labeled ? id.c_str() : input->GetLabel().c_str(), &input->editValue, 0, 0);
            input->editing = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                input->SetValue(std::clamp(input->editValue, input->GetMin(), input->GetMax()));
            }
            break;
        }
        case GUI::GUIComponentType::OPTION: {
            auto* option = static_cast<GUI::OptionButton*>(component);
            if (!labeled)
            {
                if (DrawStyledButton(option->GetDisplayText() + id, ImVec2(controlWidth, ImGui::GetFrameHeight())))
                {
                    option->Click();
                }
                break;
            }

            // Dropdown listing every option, current one highlighted.
            ImGui::SetNextItemWidth(controlWidth);
            if (ImGui::BeginCombo(id.c_str(), option->GetValueText().c_str(), ImGuiComboFlags_HeightLarge))
            {
                const std::vector<std::string>& options = option->GetOptions();
                size_t current = option->GetIndex();
                for (size_t i = 0; i < options.size(); i++)
                {
                    bool selected = i == current;
                    if (ImGui::Selectable(options[i].c_str(), selected)) option->Select(i);
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            break;
        }
    }

    ImGui::PopStyleVar();
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
    // Keyboard/gamepad navigation only works in the focused window, so a
    // newly opened screen takes focus (its first control gets it below).
    static const GUI::Screen* lastScreen = nullptr;
    bool screenJustOpened = lastScreen != &screen;
    lastScreen = &screen;
    if (screenJustOpened) ImGui::SetNextWindowFocus();
    // No window background — the engine's world passes draw the 3D scene
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

    // Vertically centered: title + content as one block, using the height
    // that block measured last frame (ImGui only knows it after drawing) —
    // a newly opened screen sits at the top for its first frame only.
    static std::unordered_map<const GUI::Screen*, float> lastContentHeight;
    float contentTop = std::max(24.0f, (ImGui::GetWindowHeight() - lastContentHeight[&screen]) * 0.5f);

    ImVec2 titleSize = ImGui::CalcTextSize(screen.title.c_str());
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - titleSize.x) * 0.5f, contentTop));
    // Screens with custom content draw their own title (the inventory's is
    // inside the vanilla panel).
    if (!screen.customContent)
    {
        constexpr float logoSize = 128.0f;
        ImTextureID logo = screen.showLogo ? TextureOf(logoIcon) : 0;
        if (logo != 0)
        {
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - logoSize) * 0.5f);
            ImGui::Image(logo, ImVec2(logoSize, logoSize));
        }
        else
        {
            ImGui::GetFont()->Scale = 1.15f;
            ImGui::Text("%s", screen.title.c_str());
            ImGui::GetFont()->Scale = 1.00f;
        }

        ImGui::Dummy(ImVec2(0.0f, 16.0f));
    }

    // customContent (see Screen::customContent's own comment) replaces the
    // generic centered TEXT/BUTTON/INPUT stack entirely for a screen that
    // needs its own layout — the inventory screen's slot grid, so far.
    if (screen.customContent)
    {
        screen.customContent();
    }
    else
    {
        // Labeled controls: label column as wide as this screen's longest
        // label, laid out in screen.columns columns. Buttons/text span the
        // middle on their own row.
        constexpr float buttonWidth = 240.0f;
        constexpr float controlWidth = 160.0f;
        constexpr float columnGap = 16.0f;
        constexpr float labelGap = 12.0f;

        float labelWidth = 0.0f;
        for (GUI::GUIComponent* component : screen.components)
        {
            if (IsLabeledControl(component))
            {
                labelWidth = std::max(labelWidth, ImGui::CalcTextSize(component->GetLabel().c_str()).x + labelGap);
            }
        }
        int columns = std::max(1, screen.columns);
        float cellWidth = labelWidth + controlWidth;
        float gridLeft = (ImGui::GetWindowWidth() - (columns * cellWidth + (columns - 1) * columnGap)) * 0.5f;

        int cell = 0; // position within the current run of labeled controls
        bool first = true;
        const GUI::GUIComponent* hovered = nullptr;
        ImVec2 hoveredMin, hoveredMax;
        bool hoveredOnLeft = false;
        for (GUI::GUIComponent* component : screen.components)
        {
            float width = buttonWidth;
            int column = 0;
            if (IsLabeledControl(component))
            {
                column = cell % columns;
                if (column == 0) ImGui::SetCursorPosX(gridLeft);
                else ImGui::SameLine(gridLeft + column * (cellWidth + columnGap));
                width = cellWidth;
                cell++;
            }
            else
            {
                cell = 0;
                if (component->GetType() == GUI::GUIComponentType::TEXT)
                {
                    width = ImGui::CalcTextSize(component->GetLabel().c_str()).x;
                }
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            }

            ImVec2 cellMin = ImGui::GetCursorScreenPos();
            RenderComponent(component, width, IsLabeledControl(component) ? labelWidth : 0.0f);
            ImVec2 cellMax(cellMin.x + width, ImGui::GetItemRectMax().y);

            if (!component->GetTooltip().empty() && ImGui::IsMouseHoveringRect(cellMin, cellMax))
            {
                hovered = component;
                hoveredMin = cellMin;
                hoveredMax = cellMax;
                // Outer side: left for the left column of a multi-column grid.
                hoveredOnLeft = columns > 1 && IsLabeledControl(component) && column == 0;
            }

            if (first && component->GetType() != GUI::GUIComponentType::TEXT)
            {
                if (screenJustOpened) ImGui::SetItemDefaultFocus();
                first = false;
            }
        }

        // Not while a dropdown is open; it would cover it.
        if (hovered != nullptr && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        {
            DrawSideTooltip(hovered->GetTooltip(), hoveredMin, hoveredMax, hoveredOnLeft);
        }
    }

    lastContentHeight[&screen] = ImGui::GetCursorPosY() - contentTop;
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
        // Confirms Enter/InputText itself fired, separate from whether the
        // network layer then actually queued anything (see SendChatMessage/
        // SendChatCommand's own new logging) — closes the last gap in
        // "chat does nothing and nothing logs" being fully undiagnosable.
        Log::Info(std::string("Chat input submitted: '") + chatInputBuffer + "'");
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
    // Vanilla GUI geometry in GUI units: 1 unit = 1 texture pixel, and
    // Graphics.GUIScale scales the whole thing (see ApplyGUIScale).
    constexpr int HOTBAR_SLOTS = 9;
    constexpr int ARMOR_SLOTS = 4;
    constexpr float ITEM_SIZE = 16.0f;          // Item icon area inside a slot.
    constexpr float HOTBAR_SLOT_STRIDE = 20.0f; // Slot spacing in hud/hotbar.png.
    constexpr float HOTBAR_WIDTH = 182.0f;
    constexpr float HOTBAR_HEIGHT = 22.0f;
    constexpr float ARMOR_WIDTH = ARMOR_SLOTS * HOTBAR_SLOT_STRIDE + 2.0f; // The hotbar texture cut down to 4 slots.
    constexpr float HUD_BOTTOM_MARGIN = 2.0f; // hotbar's own inset from the bottom edge
    constexpr float PANEL_GAP = 10.0f;         // hotbar <-> armor / hotbar <-> state-panel spacing

    // Health and hunger side by side (not stacked) spanning the hotbar's own
    // width; saturation isn't a third row — vanilla treats it as a "buffer"
    // layered on top of the hunger value rather than its own stat, so it's
    // drawn as a semi-transparent overlay on the hunger bar instead (see
    // DrawStatBar's overlayFrac).
    constexpr float BAR_HEIGHT = 22.0f;
    constexpr float BAR_GAP = 4.0f;
    constexpr float BAR_WIDTH = 130.0f; // Wide enough for the "Health  20.0 / 20" text; wider than half the hotbar.
    constexpr float BAR_PADDING = 8.0f;
    constexpr float STATUS_PANEL_WIDTH = BAR_WIDTH * 2.0f + BAR_GAP + BAR_PADDING * 2.0f;
    constexpr float STATUS_PANEL_HEIGHT = BAR_HEIGHT + BAR_PADDING * 2.0f;
    constexpr float STATUS_PANEL_GAP = 12.0f;  // status panel <-> hotbar spacing (6 extra for the future XP bar)

    // Draws one item's icon plus (when > 1) its stack count at `pos` (the
    // top-left of the 16x16 item area; the slot art itself comes from the
    // background texture). `icon` is a GUIController::IconForItem() result;
    // 0 (empty slot, unresolved item id, or no icon for this item) draws
    // nothing.
    void DrawItemIcon(ImDrawList* drawList, ImVec2 pos, ImTextureID icon, uint8_t count)
    {
        if (icon == 0) return;

        ImVec2 max(pos.x + Art(ITEM_SIZE), pos.y + Art(ITEM_SIZE));
        if (linearIconTextures.count(icon) != 0) drawList->AddImage(icon, pos, max);
        else AddImageNearest(drawList, icon, pos, max);

        if (count > 1)
        {
            // Vanilla's count: 8 units tall, right-aligned to the slot's
            // bottom-right, with a gray shadow.
            const float COUNT_FONT_SIZE = Art(8.0f);
            char text[8];
            std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(count));
            ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(COUNT_FONT_SIZE, FLT_MAX, 0.0f, text);
            ImVec2 textPos(pos.x + Art(ITEM_SIZE + 1.0f) - textSize.x, pos.y + Art(ITEM_SIZE + 1.0f) - COUNT_FONT_SIZE);
            drawList->AddText(ImGui::GetFont(), COUNT_FONT_SIZE, ImVec2(textPos.x + Art(1.0f), textPos.y + Art(1.0f)),
                IM_COL32(63, 63, 63, 255), text);
            drawList->AddText(ImGui::GetFont(), COUNT_FONT_SIZE, textPos, IM_COL32(255, 255, 255, 255), text);
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
    return ResolveAnchor(GUI::ScreenAnchor::BOTTOM, ImVec2(Art(HOTBAR_WIDTH), Art(HOTBAR_HEIGHT)), ImVec2(0.0f, Art(HUD_BOTTOM_MARGIN)));
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

    // Bar backgrounds first, so no item icon ends up under a later bar.
    // All vanilla-sized geometry goes through Art() (see g_artScale).
    ImTextureID hotbarTexture = TextureOf(hotbarIcon);
    ImVec2 hotbarMax(hotbarPos.x + Art(HOTBAR_WIDTH), hotbarPos.y + Art(HOTBAR_HEIGHT));
    if (hotbarTexture != 0) AddImageNearest(drawList, hotbarTexture, hotbarPos, hotbarMax);
    else drawList->AddRectFilled(hotbarPos, hotbarMax, IM_COL32(0, 0, 0, 120));

    // Armor: the hotbar texture cut down to 4 slots (its first 81 columns
    // plus the 1-column right border).
    ImVec2 armorPos(hotbarPos.x - PANEL_GAP - Art(ARMOR_WIDTH), hotbarPos.y);
    if (hotbarTexture != 0)
    {
        constexpr float bodyWidth = ARMOR_WIDTH - 1.0f;
        AddImageNearest(drawList, hotbarTexture, armorPos, ImVec2(armorPos.x + Art(bodyWidth), armorPos.y + Art(HOTBAR_HEIGHT)),
            ImVec2(0.0f, 0.0f), ImVec2(bodyWidth / HOTBAR_WIDTH, 1.0f));
        AddImageNearest(drawList, hotbarTexture, ImVec2(armorPos.x + Art(bodyWidth), armorPos.y),
            ImVec2(armorPos.x + Art(ARMOR_WIDTH), armorPos.y + Art(HOTBAR_HEIGHT)),
            ImVec2((HOTBAR_WIDTH - 1.0f) / HOTBAR_WIDTH, 0.0f), ImVec2(1.0f, 1.0f));
    }
    else
    {
        drawList->AddRectFilled(armorPos, ImVec2(armorPos.x + Art(ARMOR_WIDTH), armorPos.y + Art(HOTBAR_HEIGHT)),
            IM_COL32(0, 0, 0, 120));
    }

    for (int i = 0; i < HOTBAR_SLOTS; i++)
    {
        ImVec2 itemPos(hotbarPos.x + Art(3.0f + i * HOTBAR_SLOT_STRIDE), hotbarPos.y + Art(3.0f));
        const ItemStack& item = hotbarItems[static_cast<size_t>(i)];
        DrawItemIcon(drawList, itemPos, IconForItem(item), item.count);
    }

    for (int i = 0; i < ARMOR_SLOTS; i++)
    {
        ImVec2 itemPos(armorPos.x + Art(3.0f + i * HOTBAR_SLOT_STRIDE), armorPos.y + Art(3.0f));
        const ItemStack& item = armorItems[static_cast<size_t>(i)];
        DrawItemIcon(drawList, itemPos, IconForItem(item), item.count);
    }

    // Held-slot frame, drawn over the item.
    ImTextureID selectionTexture = TextureOf(hotbarSelectionIcon);
    ImVec2 selectionPos(hotbarPos.x + Art(-1.0f + selectedSlot * HOTBAR_SLOT_STRIDE), hotbarPos.y + Art(-1.0f));
    ImVec2 selectionMax(selectionPos.x + Art(24.0f), selectionPos.y + Art(23.0f));
    if (selectionTexture != 0) AddImageNearest(drawList, selectionTexture, selectionPos, selectionMax);
    else drawList->AddRect(selectionPos, selectionMax, IM_COL32(255, 255, 255, 255));

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
    // Centered over the hotbar (the bars are wider than it).
    ImVec2 panelPos(
        hotbarPos.x + (Art(HOTBAR_WIDTH) - STATUS_PANEL_WIDTH) * 0.5f,
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
    ImVec2 panelPos(hotbarPos.x + Art(HOTBAR_WIDTH) + PANEL_GAP, hotbarPos.y);

    ImGui::SetNextWindowPos(panelPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelWidth, Art(HOTBAR_HEIGHT)), ImGuiCond_Always);
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
    // TickLoop's real sprint state, not the key: a sprint keeps going after
    // the key is released, and ends on a sprint hit/wall/low food with it
    // still held.
    bool sprinting = state->tickLoop->GetMovementSnapshot().sprinting;
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
    ImGui::SetCursorPos(ImVec2((panelWidth - textSize.x) * 0.5f, (Art(HOTBAR_HEIGHT) - textSize.y) * 0.5f));
    ImGui::TextColored(ImVec4(0.35f, 0.92f, 1.00f, 1.00f), "%s", label);

    ImGui::End();
}

void GUIController::RenderInventoryScreen()
{
    std::array<ItemStack, InventoryManager::ARMOR_SIZE> armorItems;
    std::array<ItemStack, 5> craftingItems; // 0 = result, 1-4 = 2x2 grid.
    ItemStack offhandItem;
    std::array<ItemStack, InventoryManager::MAIN_INVENTORY_SIZE> mainItems;
    std::array<ItemStack, HOTBAR_SLOTS> hotbarItems;
    std::vector<ItemStack> containerItems;
    int32_t activeWindowId;
    int32_t activeWindowType;
    ItemStack carriedItem;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        for (size_t i = 0; i < armorItems.size(); i++) armorItems[i] = state->inventory.armor.Get(i);
        offhandItem = state->inventory.offhand.Get(0);
        for (size_t i = 0; i < craftingItems.size(); i++) craftingItems[i] = state->inventory.crafting.Get(i);
        for (size_t i = 0; i < mainItems.size(); i++) mainItems[i] = state->inventory.mainInventory.Get(i);
        for (size_t i = 0; i < hotbarItems.size(); i++) hotbarItems[i] = state->inventory.hotbar.Get(i);
        containerItems = state->inventory.containerSlots;
        activeWindowId = state->inventory.activeWindowId;
        activeWindowType = state->inventory.activeWindowType;
        carriedItem = state->inventory.carriedItem;
    }

    // Vanilla layout, in GUI units (1 unit = 1 texture pixel; Graphics.GUIScale
    // multiplies it). Slot coordinates are the top-left of the 16x16 item
    // area, relative to the panel — the background texture already has the
    // 18x18 slot art drawn behind them.
    struct SlotDef { int32_t index; float x; float y; };
    std::vector<SlotDef> slots;

    constexpr float PANEL_WIDTH = 176.0f;
    constexpr float TEXTURE_SIZE = 256.0f;
    float panelHeight = 166.0f;
    const StandaloneIcon* background = &inventoryBackground;
    int genericRows = 0; // > 0 selects generic_54's split top/bottom drawing.
    float titleX = 8.0f;
    bool drawTitle = activeWindowId != 0;

    // Player inventory + hotbar, placed by their first protocol index.
    auto addPlayerSlots = [&](int32_t firstMain, float mainY, int32_t firstHotbar, float hotbarY)
    {
        for (int row = 0; row < 3; row++)
        {
            for (int col = 0; col < 9; col++)
            {
                slots.push_back({ firstMain + row * 9 + col, 8.0f + col * 18.0f, mainY + row * 18.0f });
            }
        }
        for (int col = 0; col < 9; col++) slots.push_back({ firstHotbar + col, 8.0f + col * 18.0f, hotbarY });
    };

    int32_t containerCount = static_cast<int32_t>(containerItems.size());
    if (activeWindowId == 0)
    {
        for (int i = 0; i < 4; i++) slots.push_back({ 5 + i, 8.0f, 8.0f + i * 18.0f });
        slots.push_back({ 45, 77.0f, 62.0f });
        for (int row = 0; row < 2; row++)
        {
            for (int col = 0; col < 2; col++) slots.push_back({ 1 + row * 2 + col, 98.0f + col * 18.0f, 18.0f + row * 18.0f });
        }
        slots.push_back({ 0, 154.0f, 28.0f });
        addPlayerSlots(9, 84.0f, 36, 142.0f);
    }
    else if (activeWindowType == InventoryManager::CRAFTING_MENU_TYPE && containerCount >= 10)
    {
        background = &craftingBackground;
        titleX = 29.0f;
        slots.push_back({ 0, 124.0f, 35.0f });
        for (int row = 0; row < 3; row++)
        {
            for (int col = 0; col < 3; col++) slots.push_back({ 1 + row * 3 + col, 30.0f + col * 18.0f, 17.0f + row * 18.0f });
        }
        addPlayerSlots(containerCount, 84.0f, containerCount + 27, 142.0f);
    }
    else if (activeWindowType == InventoryManager::FURNACE_MENU_TYPE && containerCount >= 3)
    {
        background = &furnaceBackground;
        slots.push_back({ 0, 56.0f, 17.0f });
        slots.push_back({ 1, 56.0f, 53.0f });
        slots.push_back({ 2, 116.0f, 35.0f });
        addPlayerSlots(containerCount, 84.0f, containerCount + 27, 142.0f);
    }
    else
    {
        // Chest-style grid: also the fallback for any menu without its own layout.
        background = &genericBackground;
        genericRows = std::max(1, (containerCount + 8) / 9);
        panelHeight = 114.0f + genericRows * 18.0f;
        for (int32_t i = 0; i < containerCount; i++) slots.push_back({ i, 8.0f + (i % 9) * 18.0f, 18.0f + (i / 9) * 18.0f });
        addPlayerSlots(containerCount, panelHeight - 82.0f, containerCount + 27, panelHeight - 24.0f);
    }

    // The item at a protocol slot index in the layout above.
    static const ItemStack emptyItem{};
    auto itemAt = [&](int32_t index) -> const ItemStack&
    {
        if (activeWindowId == 0)
        {
            if (index >= 0 && index <= 4) return craftingItems[static_cast<size_t>(index)];
            if (index >= 5 && index <= 8) return armorItems[static_cast<size_t>(index - 5)];
            if (index >= 9 && index <= 35) return mainItems[static_cast<size_t>(index - 9)];
            if (index >= 36 && index <= 44) return hotbarItems[static_cast<size_t>(index - 36)];
            if (index == 45) return offhandItem;
            return emptyItem;
        }
        if (index < 0) return emptyItem;
        if (index < containerCount) return containerItems[static_cast<size_t>(index)];
        int32_t playerSlot = index - containerCount;
        if (playerSlot < 27) return mainItems[static_cast<size_t>(playerSlot)];
        if (playerSlot < 36) return hotbarItems[static_cast<size_t>(playerSlot - 27)];
        return emptyItem;
    };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float panelX = std::floor(ImGui::GetWindowPos().x + (ImGui::GetWindowWidth() - Art(PANEL_WIDTH)) * 0.5f);
    float panelY = std::floor(ImGui::GetCursorScreenPos().y);

    // Background texture.
    ImTextureID backgroundTexture = TextureOf(*background);
    if (backgroundTexture == 0)
    {
        drawList->AddRectFilled(ImVec2(panelX, panelY), ImVec2(panelX + Art(PANEL_WIDTH), panelY + Art(panelHeight)),
            IM_COL32(198, 198, 198, 255));
    }
    else if (genericRows > 0)
    {
        float topHeight = 17.0f + genericRows * 18.0f;
        AddImageNearest(drawList, backgroundTexture, ImVec2(panelX, panelY), ImVec2(panelX + Art(PANEL_WIDTH), panelY + Art(topHeight)),
            ImVec2(0.0f, 0.0f), ImVec2(PANEL_WIDTH / TEXTURE_SIZE, topHeight / TEXTURE_SIZE));
        AddImageNearest(drawList, backgroundTexture, ImVec2(panelX, panelY + Art(topHeight)),
            ImVec2(panelX + Art(PANEL_WIDTH), panelY + Art(panelHeight)),
            ImVec2(0.0f, 126.0f / TEXTURE_SIZE), ImVec2(PANEL_WIDTH / TEXTURE_SIZE, 222.0f / TEXTURE_SIZE));
    }
    else
    {
        AddImageNearest(drawList, backgroundTexture, ImVec2(panelX, panelY), ImVec2(panelX + Art(PANEL_WIDTH), panelY + Art(panelHeight)),
            ImVec2(0.0f, 0.0f), ImVec2(PANEL_WIDTH / TEXTURE_SIZE, panelHeight / TEXTURE_SIZE));
    }

    const float LABEL_FONT_SIZE = Art(8.0f);
    constexpr ImU32 LABEL_COLOR = IM_COL32(64, 64, 64, 255);
    if (drawTitle)
    {
        drawList->AddText(ImGui::GetFont(), LABEL_FONT_SIZE, ImVec2(panelX + Art(titleX), panelY + Art(6.0f)), LABEL_COLOR,
            inventoryScreen->title.c_str());
        drawList->AddText(ImGui::GetFont(), LABEL_FONT_SIZE, ImVec2(panelX + Art(8.0f), panelY + Art(panelHeight - 94.0f)),
            LABEL_COLOR, "Inventory");
    }

    bool hasMouseClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    uint8_t clickedButton = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 1 : 0;
    bool clickHandled = false;
    ImVec2 mouse = ImGui::GetIO().MousePos;

    auto handleSlotClick = [&](int32_t protocolIndex)
    {
        clickHandled = true;

        int32_t windowId;
        int32_t stateId;
        {
            std::lock_guard<std::mutex> lock(state->inventory.mutex);
            windowId = state->inventory.activeWindowId;
            stateId = windowId == 0 ? state->inventory.playerStateId : state->inventory.activeWindowStateId;
        }
        state->inventory.HandleSlotClick(protocolIndex, clickedButton);

        ItemStack slotItem;
        ItemStack cursorItem;
        {
            std::lock_guard<std::mutex> lock(state->inventory.mutex);
            ItemStack* slot = state->inventory.GetWindowSlot(protocolIndex);
            if (slot != nullptr) slotItem = *slot;
            cursorItem = state->inventory.carriedItem;
        }
        SendInventoryClick(state, windowId, stateId, protocolIndex, clickedButton, slotItem, cursorItem);
    };

    // Hovered slot: highlight behind the item, plus a click.
    const SlotDef* hovered = nullptr;
    for (const SlotDef& slot : slots)
    {
        float x = panelX + Art(slot.x);
        float y = panelY + Art(slot.y);
        if (mouse.x >= x && mouse.x < x + Art(ITEM_SIZE) && mouse.y >= y && mouse.y < y + Art(ITEM_SIZE))
        {
            hovered = &slot;
            break;
        }
    }
    if (hovered != nullptr && hasMouseClick) handleSlotClick(hovered->index);

    ImTextureID highlightBack = TextureOf(slotHighlightBackIcon);
    ImTextureID highlightFront = TextureOf(slotHighlightFrontIcon);
    constexpr float HIGHLIGHT_SIZE = 24.0f;
    auto drawHighlight = [&](ImTextureID texture, ImU32 fallback)
    {
        if (hovered == nullptr) return;
        ImVec2 min(panelX + Art(hovered->x - 4.0f), panelY + Art(hovered->y - 4.0f));
        if (texture != 0) AddImageNearest(drawList, texture, min, ImVec2(min.x + Art(HIGHLIGHT_SIZE), min.y + Art(HIGHLIGHT_SIZE)));
        else if (fallback != 0)
        {
            drawList->AddRectFilled(ImVec2(min.x + Art(4.0f), min.y + Art(4.0f)),
                ImVec2(min.x + Art(20.0f), min.y + Art(20.0f)), fallback);
        }
    };

    drawHighlight(highlightBack, IM_COL32(255, 255, 255, 90));
    for (const SlotDef& slot : slots)
    {
        const ItemStack& item = itemAt(slot.index);
        DrawItemIcon(drawList, ImVec2(panelX + Art(slot.x), panelY + Art(slot.y)), IconForItem(item), item.count);
    }
    drawHighlight(highlightFront, 0);

    if (hasMouseClick && !clickHandled && !carriedItem.IsEmpty())
    {
        int32_t windowId;
        int32_t stateId;
        {
            std::lock_guard<std::mutex> lock(state->inventory.mutex);
            windowId = state->inventory.activeWindowId;
            stateId = windowId == 0 ? state->inventory.playerStateId : state->inventory.activeWindowStateId;
        }
        state->inventory.HandleSlotClick(-999, clickedButton);
        ItemStack cursorItem;
        {
            std::lock_guard<std::mutex> lock(state->inventory.mutex);
            cursorItem = state->inventory.carriedItem;
        }
        SendInventoryClick(state, windowId, stateId, -999, clickedButton, ItemStack{}, cursorItem);
    }

    // The carried stack follows the cursor, drawn last so it sits on top.
    if (!carriedItem.IsEmpty())
    {
        DrawItemIcon(drawList, ImVec2(mouse.x - Art(ITEM_SIZE) * 0.5f, mouse.y - Art(ITEM_SIZE) * 0.5f),
                     IconForItem(carriedItem), carriedItem.count);
    }

    // Claim the panel's space — raw ImDrawList calls don't move ImGui's
    // layout cursor, and ImGui asserts if a window's extent grows without a
    // real item (see git history for the original assert text).
    ImGui::Dummy(ImVec2(Art(PANEL_WIDTH), Art(panelHeight)));
}

// Dark navy panels with a orange border tint, laid on top of StyleColorsDark.
// Called once at Init — component-level drawing (DrawStyledButton) pulls
// its own hand-picked colors rather than reading these back, since ImGui's
// built-in widget colors don't cover custom-drawn glow/border fills.
void GUIController::ApplyVolcanoTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.FrameRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.WindowBorderSize = 2.0f;
    style.FrameBorderSize = 2.0f;
    // Keeps lines off the font atlas so AddTextShadows only shadows text.
    style.AntiAliasedLinesUseTex = false;

    ImVec4* colors = style.Colors;
    const ImVec4 panelDark  = ImVec4(0.040f, 0.040f, 0.040f, 1.000f);
    const ImVec4 panelMid   = ImVec4(0.100f, 0.100f, 0.100f, 1.000f);
    const ImVec4 panelLight = ImVec4(0.160f, 0.160f, 0.160f, 1.000f);
    const ImVec4 orange     = ImVec4(1.000f, 0.710f, 0.063f, 1.000f); // #fbb510
    const ImVec4 orangeDim  = ImVec4(0.957f, 0.353f, 0.043f, 1.000f); // #f45a0b

    colors[ImGuiCol_WindowBg]         = panelDark;
    colors[ImGuiCol_ChildBg]          = panelDark;
    colors[ImGuiCol_PopupBg]          = panelDark;
    colors[ImGuiCol_Border]           = panelLight;
    colors[ImGuiCol_BorderShadow]     = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg]          = panelMid;
    colors[ImGuiCol_FrameBgHovered]   = panelLight;
    colors[ImGuiCol_FrameBgActive]    = panelLight;
    colors[ImGuiCol_TitleBg]          = panelDark;
    colors[ImGuiCol_TitleBgActive]    = panelDark;
    colors[ImGuiCol_TitleBgCollapsed] = panelDark;
    colors[ImGuiCol_Button]           = panelDark;
    colors[ImGuiCol_ButtonHovered]    = orange;
    colors[ImGuiCol_ButtonActive]     = orangeDim;
    colors[ImGuiCol_CheckMark]        = orange;
    colors[ImGuiCol_SliderGrab]       = orange;
    colors[ImGuiCol_SliderGrabActive] = orangeDim;
    colors[ImGuiCol_Separator]        = orangeDim;
    colors[ImGuiCol_SeparatorHovered] = orangeDim;
    colors[ImGuiCol_Text]             = ImVec4(0.950f, 0.950f, 0.950f, 1.000f);
    colors[ImGuiCol_TextDisabled]     = ImVec4(0.600f, 0.600f, 0.600f, 1.000f);
}

// See header: flat-glow button drawn with ImDrawList so it doesn't depend
// on ImGui's built-in button skin. An InvisibleButton underneath handles
// click/hover/active state; everything visible is drawn manually on top.
bool GUIController::DrawStyledButton(const std::string& label, ImVec2 size)
{
    if (size.x <= 0.0f) size.x = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + 32.0f;
    if (size.y <= 0.0f) size.y = ImGui::GetFrameHeight() + 8.0f;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    // InvisibleButton's own return covers keyboard/gamepad activation too;
    // IsItemClicked alone is mouse-only. Nav focus draws like hover.
    bool clicked = ImGui::InvisibleButton(label.c_str(), size);
    bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    bool active = ImGui::IsItemActive();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 rectMax = ImVec2(pos.x + size.x, pos.y + size.y);
    constexpr float rounding = 6.0f;

    // Only filled when active.
    ImU32 fill = active
        ? IM_COL32(244, 90, 11, 255)
        : hovered
            ? IM_COL32(16, 16, 16, 255)
            : IM_COL32(16, 16, 16, 255);
    // When not active but hovered, it's just the border.
    ImU32 border = (hovered || active)
        ? IM_COL32(244, 90, 11, 255)
        : IM_COL32(32, 32, 32, 255);

    drawList->AddRectFilled(pos, rectMax, fill, rounding);
    drawList->AddRect(pos, rectMax, border, rounding, 0, 1.5f);

    // Like ImGui's own widgets, anything after "##" is ID-only, not drawn.
    const char* textEnd = label.c_str() + std::min(label.find("##"), label.size());
    ImVec2 textSize = ImGui::CalcTextSize(label.c_str(), textEnd);
    ImVec2 textPos = ImVec2(
        pos.x + (size.x - textSize.x) * 0.5f,
        pos.y + (size.y - textSize.y) * 0.5f);
    drawList->AddText(textPos, IM_COL32(235, 248, 255, 255), label.c_str(), textEnd);

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

void GUIController::CloseInventoryWindow()
{
    int32_t windowId = 0;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        windowId = state->inventory.activeWindowId;
        if (windowId != 0)
        {
            state->inventory.activeWindowId = 0;
            state->inventory.activeWindowType = 0;
            state->inventory.activeWindowTitle.clear();
            state->inventory.containerSlots.clear();
            state->inventory.carriedItem = ItemStack{};
        }
        state->inventory.openWindowPending = false;
        state->inventory.closeWindowPending = false;
    }
    if (windowId != 0) SendInventoryClose(state, windowId);
    inventoryScreen->title = "Inventory";
}

// Show a screen fullscreen, hiding the HUD and releasing the mouse cursor
// so ImGui (rather than the fly-cam) drives it. GUIController runs on the
// render thread, but GLFW only guarantees glfwSetInputMode is safe from the
// main (window-owning) thread — see GlobalState::pendingCursorMode, applied
// by VolcanoClient.cpp's main loop.
void GUIController::OpenScreen(GUI::Screen* screen)
{
    if (activeScreen == inventoryScreen && screen != inventoryScreen) CloseInventoryWindow();
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
    if (activeScreen == inventoryScreen) CloseInventoryWindow();
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
