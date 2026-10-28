#include "GUIController.hpp"
#include "Logger.hpp"
#include "audio/SoundRegistry.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <imgui.h>
#include <imgui_internal.h>
#include <cfloat>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>
#include <cstring>
#include <unordered_set>
#include <stb_image.h>
#include "../../resources/ImageLoad.hpp"
#include "gui/Animation.hpp"
#include "gui/screens/InventoryScreen.hpp"
#include "gui/models/GUIComponent.hpp"
#include "gui/models/Button.hpp"
#include "gui/models/Chat.hpp"
#include "gui/models/NumberInput.hpp"
#include "gui/models/OptionButton.hpp"
#include "gui/models/Slider.hpp"
#include "inventory/ItemRegistry.hpp"
#include "inventory/ToolStats.hpp"
#include "TickLoop.hpp"
#include "ModLoader.hpp"
#include "DropKey.hpp"
#include "SystemInfo.hpp"
#include "network/NetworkStats.hpp"
#include "network/PacketIds.hpp"
#include "renderer/engine/RenderEngine.hpp"
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
// header drags in asio.hpp, which on Windows pulls in <windows.h> -
// whose CreateWindowA/CreateWindow macros collide with
// GUIController::CreateWindow below. This one free function is all
// GUIController actually needs from it.
void SendChatMessage(GlobalState* state, const std::string& message);
void SendChatCommand(GlobalState* state, const std::string& command);
void SendRespawnRequest(GlobalState* state);
void SendInventoryClick(GlobalState* state, int32_t windowId, int32_t stateId, int32_t slot,
                        uint8_t button, const ItemStack& slotItem, const ItemStack& carriedItem);
void SendInventoryAction(GlobalState* state, int32_t windowId, int32_t stateId, int32_t slot,
                         uint8_t button, int32_t mode, const ItemStack& carriedItem);
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

    // Every standard control (main menu buttons, inputs, dropdowns) is drawn with
    // frames this much taller than ImGui's default, so they all match the main
    // menu's buttons. Controls nest (RenderComponent -> DrawButton), so the
    // padding is only added once.
    constexpr float CONTROL_EXTRA_PADDING = 4.0f;
    bool controlPaddingActive = false;

    struct ControlStyle {
        bool owner;
        ControlStyle() : owner(!controlPaddingActive)
        {
            if (!owner) return;
            controlPaddingActive = true;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y + CONTROL_EXTRA_PADDING));
        }
        ~ControlStyle()
        {
            if (!owner) return;
            ImGui::PopStyleVar();
            controlPaddingActive = false;
        }
        ControlStyle(const ControlStyle&) = delete;
        ControlStyle& operator=(const ControlStyle&) = delete;
    };

    // Vanilla's font line height (9 units) at the base GUI scale.
    constexpr float FONT_SIZE = 18.0f;


    // Text this dark gets no shadow (labels on the light vanilla panels): a
    // black shadow behind it only muddies it.
    bool IsDarkText(ImU32 color)
    {
        int sum = static_cast<int>((color >> IM_COL32_R_SHIFT) & 0xFF)
            + static_cast<int>((color >> IM_COL32_G_SHIFT) & 0xFF)
            + static_cast<int>((color >> IM_COL32_B_SHIFT) & 0xFF);
        return sum < 3 * 96;
    }
}

GlobalState* GUIController::state = nullptr;
GLFWwindow* GUIController::windowHandle = nullptr;
const TextureManager* GUIController::textureManager = nullptr;
std::vector<GUI::GUIWindow*> GUIController::windows = {};
GUI::Screen* GUIController::activeScreen = nullptr;
std::unique_ptr<GUI::ControlRenderer> GUIController::controlRenderer;
std::unique_ptr<GUI::HudRenderer> GUIController::hudRenderer;
bool GUIController::hudDrawn = false;
const GUI::Screen* GUIController::pincelScreen = nullptr;
ImGuiVulkanContext GUIController::imguiContext;
bool GUIController::initialized = false;
float GUIController::fps = 0.0f;
float GUIController::frameTime = 0.0f;
int GUIController::frameCount = 0;
float GUIController::fpsTimer = 0.0f;
GUI::Screen* GUIController::pauseScreen = nullptr;
GUI::Screen* GUIController::deathScreen = nullptr;
GUI::GUIComponent* GUIController::deathMessageText = nullptr;
GUI::Screen* GUIController::inventoryScreen = nullptr;
bool GUIController::chatInputOpen = false;
bool GUIController::chatInputJustOpened = false;
char GUIController::chatInputBuffer[256] = {};
std::unordered_map<uint16_t, GUIController::ItemIcon> GUIController::itemIconCache;
std::unordered_map<uint16_t, GUIController::ModelIcon> GUIController::modelIconCache;
std::unordered_map<uint16_t, GUI::Sprite> GUIController::itemSprites;
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
GUIController::StandaloneIcon GUIController::heartContainerIcon;
GUIController::StandaloneIcon GUIController::heartFullIcon;
GUIController::StandaloneIcon GUIController::heartHalfIcon;
GUIController::StandaloneIcon GUIController::foodEmptyIcon;
GUIController::StandaloneIcon GUIController::foodFullIcon;
GUIController::StandaloneIcon GUIController::foodHalfIcon;
GUIController::StandaloneIcon GUIController::experienceBackgroundIcon;
GUIController::StandaloneIcon GUIController::experienceProgressIcon;
GUIController::StandaloneIcon GUIController::armorEmptyIcon;
GUIController::StandaloneIcon GUIController::armorFullIcon;
GUIController::StandaloneIcon GUIController::armorHalfIcon;
GUIController::StandaloneIcon GUIController::saturationOutlineIcon;

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
    io.Fonts->AddFontFromFileTTF("resources/fonts/Minecraft.ttf", FONT_SIZE, &fontConfig);

    // System fonts merged in as fallbacks for characters Minecraft.ttf lacks
    // (box drawing, symbols, emoji in chat); glyphs load on demand.
    for (const std::string& path : Platform::OS::GetFallbackFontPaths())
    {
        ImFontConfig fallbackConfig;
        fallbackConfig.MergeMode = true;
        fallbackConfig.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(path.c_str(), FONT_SIZE, &fallbackConfig);
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

    // Screens made of plain components are drawn by Pincel; if it can't start, ImGui draws them as before.
    controlRenderer = std::make_unique<GUI::ControlRenderer>(state);
    if (!controlRenderer->Init(renderPass))
    {
        Log::Error("GUIController: Pincel could not start, drawing screens with ImGui");
        controlRenderer.reset();
    }
    hudRenderer = std::make_unique<GUI::HudRenderer>(state);
    if (!hudRenderer->Init(renderPass))
    {
        Log::Error("GUIController: Pincel could not start the HUD");
        hudRenderer.reset();
    }

    // Setup style
    ImGui::StyleColorsDark();
    ApplyVolcanoTheme();

    // The F3 overlay's built-in entries (mods add theirs via SetDebugDisplay).
    RegisterBuiltinDebugDisplays();

    // Death screen
    deathScreen = CreateScreen("You Died!", /* closable */ false);
    deathMessageText = new GUI::GUIComponent(GUI::GUIComponentType::TEXT, "");
    auto* respawnButton = new GUI::Button("Respawn");
    respawnButton->AddClickHandler(new std::function<void()>([] {
        SendRespawnRequest(state);
        CloseScreen();
    }));
    deathScreen->components.push_back(deathMessageText);
    deathScreen->components.push_back(respawnButton);

    // Inventory screen
    inventoryScreen = GUI::CreateInventoryScreen(state);

    LoadStandaloneIcons();

    Log::Info("GUI Controller initialized with ImGui");
    initialized = true;
}

// Loads every standalone HUD/GUI icon. Used by Init() and ReloadTextures().
void GUIController::LoadStandaloneIcons()
{
    // Crosshair
    crosshairIcon = LoadStandaloneIcon("resources/crosshair.png");
    crosshairEntityIcon = LoadStandaloneIcon("resources/crosshair-entity.png");

    const std::string guiTextures = "assets/minecraft/textures/gui/"; // Logical pack path.
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
    heartContainerIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/heart/container.png");
    heartFullIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/heart/full.png");
    heartHalfIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/heart/half.png");
    foodEmptyIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/food_empty.png");
    foodFullIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/food_full.png");
    foodHalfIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/food_half.png");
    experienceBackgroundIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/experience_bar_background.png");
    experienceProgressIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/experience_bar_progress.png");
    armorEmptyIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/armor_empty.png"); 
    armorFullIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/armor_full.png"); 
    armorHalfIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/armor_half.png"); 
    saturationOutlineIcon = LoadStandaloneIcon(guiTextures + "sprites/hud/food_empty.png", IM_COL32(255, 205, 40, 255));
}

// Destroys every cached/standalone icon. removeDescriptors frees the ImGui
// descriptor sets too (needed while the backend stays alive).
void GUIController::FreeIcons(bool removeDescriptors)
{
    auto removeSet = [&](VkDescriptorSet set)
    {
        if (removeDescriptors && set != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(set);
    };

    for (auto& [layer, icon] : itemIconCache) {
        removeSet(icon.descriptorSet);
        if (icon.view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), icon.view, nullptr);
    }
    itemIconCache.clear();

    for (auto& [itemId, entry] : modelIconCache) {
        removeSet(entry.icon.descriptorSet);
        if (entry.icon.view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), entry.icon.view, nullptr);
        if (entry.icon.image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, entry.icon.image, entry.icon.allocation);
    }
    modelIconCache.clear();
    itemSprites.clear();
    linearIconTextures.clear();

    // Unlike itemIconCache's views above, these own the whole image (see
    // StandaloneIcon's own comment) - the view alone isn't enough to free.
    for (StandaloneIcon* icon : { &crosshairIcon, &crosshairEntityIcon, &attackIndicatorBackgroundIcon,
            &attackIndicatorProgressIcon, &hotbarIcon, &hotbarSelectionIcon,
            &slotHighlightBackIcon, &slotHighlightFrontIcon, &inventoryBackground, &craftingBackground,
            &furnaceBackground, &genericBackground, &logoIcon, &heartContainerIcon, &heartFullIcon,
            &heartHalfIcon, &foodEmptyIcon, &foodFullIcon, &foodHalfIcon, &experienceBackgroundIcon, &experienceProgressIcon, &armorEmptyIcon, &armorFullIcon, &armorHalfIcon, &saturationOutlineIcon }) {
        removeSet(icon->descriptorSet);
        if (icon->view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), icon->view, nullptr);
        if (icon->image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, icon->image, icon->allocation);
        *icon = StandaloneIcon{};
    }
}

// Rebuilds every icon after a resource reload. Call with the GPU idle and no
// ImGui frame open; `textureManager` must already hold the new atlas.
void GUIController::ReloadTextures(const TextureManager* textureMgr)
{
    if (!initialized) return;
    textureManager = textureMgr;
    FreeIcons(true);
    LoadStandaloneIcons();
    if (controlRenderer != nullptr) controlRenderer->ReloadResources();
    if (hudRenderer != nullptr) hudRenderer->ReloadResources();
}

void GUIController::Shutdown()
{
    if (!initialized) return;

    if (hudRenderer != nullptr)
    {
        hudRenderer->Shutdown();
        hudRenderer.reset();
    }
    if (controlRenderer != nullptr)
    {
        controlRenderer->Shutdown();
        controlRenderer.reset();
    }
    ImGuiVulkan::Shutdown(imguiContext);
    FreeIcons(false);

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
// Vulkan failure
ImTextureID GUIController::GetItemIcon(uint16_t textureLayer)
{
    auto cached = itemIconCache.find(textureLayer);
    if (cached != itemIconCache.end()) {
        return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(cached->second.descriptorSet));
    }

    ItemIcon icon;

    // A narrow 2D slice of TextureManager::array's single 2D_ARRAY image -
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

const GUI::Sprite* GUIController::GetItemSprite(const ItemStack& item)
{
    if (item.IsEmpty()) return nullptr;
    uint16_t id = static_cast<uint16_t>(item.itemId);

    auto found = itemSprites.find(id);
    if (found == itemSprites.end())
    {
        GUI::Sprite sprite;
        const ItemRegistry::ItemTypeInfo* info = ItemRegistry::Lookup(item.itemId);
        if (info != nullptr)
        {
            // The same choice IconForItem makes: the 3D model's icon if it has one, else the flat sprite.
            ModelIconForItem(id, info->name);
            const StandaloneIcon& model = modelIconCache[id].icon;
            if (model.image != VK_NULL_HANDLE)
            {
                sprite = GUI::Sprite::FromGpuImage(model.image, 0, ItemRenderer::GUI_ICON_SIZE, ItemRenderer::GUI_ICON_SIZE);
            }
            else if (info->hasTexture && textureManager != nullptr)
            {
                sprite = GUI::Sprite::FromGpuImage(textureManager->array.image, info->textureLayer,
                    textureManager->LayerSize(), textureManager->LayerSize());
            }
        }
        found = itemSprites.emplace(id, std::move(sprite)).first;
    }
    return found->second.IsEmpty() ? nullptr : &found->second;
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
GUIController::StandaloneIcon GUIController::LoadStandaloneIcon(const std::string& path, uint32_t outlineColor)
{
    StandaloneIcon icon;

    // "assets/..." is a pack (logical) path; anything else is a client file on disk.
    int width = 0, height = 0, channels = 0;
    uint8_t* pixels = path.starts_with("assets/")
        ? LoadImageRGBA(path, width, height)
        : stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        Log::Error("GUIController: failed to load " + path + ", skipping.");
        return icon;
    }

    if (outlineColor != 0) {
        // The outline is the sprite's black pixels; everything else goes clear.
        for (int i = 0; i < width * height; i++) {
            uint8_t* pixel = pixels + i * 4;
            bool outline = pixel[3] > 0 && pixel[0] < 24 && pixel[1] < 24 && pixel[2] < 24;
            uint32_t value = outline ? outlineColor : 0u;
            std::memcpy(pixel, &value, 4);
        }
    }

    // Single 2D image, one layer, no mip chain
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
    // TransitionImageLayout
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
// than in a Begin/End window
void GUIController::RenderCrosshair()
{
    // Native size of both source PNGs; drawn 1:1 rather than scaled by a
    // Graphics.GUIScale-style setting, since this client has none yet.
    constexpr float CROSSHAIR_SIZE = 32.0f;

    const StandaloneIcon& active = state->lookingAtEntity.load() ? crosshairEntityIcon : crosshairIcon;
    if (active.descriptorSet == VK_NULL_HANDLE) return; // Missing/failed-to-load asset

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

    // Death takes over whatever's on screen
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

    // "T" opens the chat input box, same key Minecraft uses
    if (!chatInputOpen && activeScreen == nullptr && !state->input->IsKeyDown(GLFW_KEY_F3) // F3+T reloads resources.
        && state->input->WasActivated("OpenChat"))
    {
        OpenChatInput();
    }
    else if (!chatInputOpen && activeScreen == nullptr && state->input->WasActivated("OpenCommand"))
    {
        OpenChatInput("/");
    }
    else if (chatInputOpen && state->input->IsKeyPressed(GLFW_KEY_ESCAPE))
    {
        CloseChatInput();
    }

    // "E" toggles the inventory screen, same key Minecraft uses
    if (activeScreen == nullptr && !chatInputOpen && state->input->WasActivated("ToggleInventory"))
    {
        OpenScreen(inventoryScreen);
    }
    else if (activeScreen == inventoryScreen && state->input->WasActivated("ToggleInventory"))
    {
        CloseScreen();
    }

    // A screen of plain components is drawn by Pincel into a texture now, before the frame is recorded.
    pincelScreen = nullptr;
    if (activeScreen != nullptr && IsDrawnByPincel(*activeScreen))
    {
        controlRenderer->Update(*activeScreen, guiScale);
        pincelScreen = activeScreen;
    }

    // The HUD is drawn by Pincel into a texture now too, while no screen is open.
    hudDrawn = false;
    if (activeScreen == nullptr && hudRenderer != nullptr)
    {
        GUI::HudRenderer::ChatBox chat;
        chat.open = chatInputOpen;
        chat.justOpened = chatInputJustOpened;
        chat.initialText = chatInputBuffer;
        chatInputJustOpened = false;
        if (std::optional<std::string> submitted = hudRenderer->Update(guiScale, chat)) SubmitChatInput(*submitted);
        hudDrawn = true;
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
    }

    UpdateDebugDisplay(deltaTime);
}

// ---- Debug display (F3 overlay) ----

namespace {

// Alphabetical, ignoring case, so "movement_speed" doesn't sort after every capital.
struct CaseInsensitiveLess
{
    bool operator()(const std::string& a, const std::string& b) const
    {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
            [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    }
};

struct DebugEntry
{
    std::function<std::string()> format;
    std::string cached; // Last evaluated text; what the overlay draws between refreshes.
};

using DebugCategory = std::map<std::string, DebugEntry, CaseInsensitiveLess>;

// Guards the entries: mods may register from their own load path while the
// render thread evaluates and draws them.
std::mutex debugMutex;
std::map<std::string, DebugCategory, CaseInsensitiveLess> debugCategories;

bool debugWasVisible = false;
bool debugDirty = false;
float debugRefreshTimer = 0.0f;
constexpr float DEBUG_REFRESH_SECONDS = 0.5f; // Values change faster than they're readable, and some cost a syscall.

// Owns the std::function objects behind the built-in entries (SetDebugDisplay
// only borrows a pointer); they live for the whole process.
std::vector<std::shared_ptr<void>> builtinDebugOwners;

template <typename T>
void AddBuiltin(const std::string& category, const std::string& name, std::function<T()> update)
{
    auto owned = std::make_shared<std::function<T()>>(std::move(update));
    builtinDebugOwners.push_back(owned);
    GUIController::SetDebugDisplay<T>(category, name, owned.get());
}

std::string FormatMs(float ms)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f ms", static_cast<double>(ms));
    return text;
}

// Network counters as of one evaluation of the "Traffic" entry, to turn the
// running totals into per-second rates.
struct TrafficSample
{
    bool valid = false;
    std::chrono::steady_clock::time_point time{};
    uint64_t packetsIn = 0, packetsOut = 0, bytesIn = 0, bytesOut = 0;
};

std::string ToMB(uint64_t bytes)
{
    return std::to_string(bytes / (1024ull * 1024ull)) + " MB";
}

} // namespace

void GUIController::RegisterDebugEntry(const std::string& category, const std::string& name, std::function<std::string()> format)
{
    std::lock_guard<std::mutex> lock(debugMutex);
    debugCategories[category][name] = DebugEntry{ std::move(format), std::string() };
    debugDirty = true;
}

void GUIController::RemoveDebugDisplay(const std::string& category, const std::string& name)
{
    std::lock_guard<std::mutex> lock(debugMutex);
    auto found = debugCategories.find(category);
    if (found == debugCategories.end()) return;

    found->second.erase(name);
    if (found->second.empty()) debugCategories.erase(found);
}

// Re-evaluates every entry twice a second, and only while the overlay is
// visible - so with F3 off none of the update functions run at all.
void GUIController::UpdateDebugDisplay(float deltaTime)
{
    if (!state->debugHudVisible)
    {
        debugWasVisible = false;
        return;
    }

    debugRefreshTimer += deltaTime;
    if (debugWasVisible && !debugDirty && debugRefreshTimer < DEBUG_REFRESH_SECONDS) return;
    debugWasVisible = true;
    debugDirty = false;
    debugRefreshTimer = 0.0f;

    std::lock_guard<std::mutex> lock(debugMutex);
    for (auto& [categoryName, category] : debugCategories)
    {
        for (auto& [entryName, entry] : category)
        {
            try
            {
                entry.cached = entry.format();
            }
            catch (const std::exception& e)
            {
                entry.cached = std::string("<error: ") + e.what() + ">";
            }
        }
    }
}

// Draws the entries as "<name>: <value>" lines under their category, in two
// columns: left-aligned on the left of the screen, right-aligned on the right,
// categories in order and filling the left first. Uses the foreground draw
// list (a transparent, input-free layer over the whole screen), so nothing
// here takes focus or blocks clicks; AddTextShadows still shadows it.
void GUIController::RenderDebugOverlay()
{
    if (!state->debugHudVisible) return;

    struct Line
    {
        std::string text;
        bool header;
    };
    struct Block
    {
        std::vector<Line> lines;
    };
    std::vector<Block> blocks;
    size_t totalLines = 0;
    {
        std::lock_guard<std::mutex> lock(debugMutex);
        for (const auto& [categoryName, category] : debugCategories)
        {
            Block block;
            block.lines.push_back({ categoryName, true });
            for (const auto& [entryName, entry] : category)
            {
                // Continuation lines (after "\n" or "\n\t") are indented; ImGui has
                // no tab glyph, so a tab is drawn as 4 spaces.
                std::string text = entryName + ": " + entry.cached;
                size_t start = 0;
                while (true)
                {
                    size_t end = text.find('\n', start);
                    std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    for (size_t tab = line.find('\t'); tab != std::string::npos; tab = line.find('\t', tab + 4))
                    {
                        line.replace(tab, 1, "    ");
                    }
                    block.lines.push_back({ line, false });
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
            }
            totalLines += block.lines.size();
            blocks.push_back(std::move(block));
        }
    }
    if (blocks.empty()) return;

    // The left column takes categories until it holds at least half the lines.
    size_t leftCount = 0;
    for (size_t leftLines = 0; leftCount < blocks.size() && leftLines * 2 < totalLines; leftCount++)
    {
        leftLines += blocks[leftCount].lines.size();
    }

    constexpr float margin = 8.0f;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImU32 headerColor = IM_COL32(255, 213, 79, 255);
    const ImU32 textColor = IM_COL32(235, 235, 235, 255);

    float leftY = margin, rightY = margin;
    for (size_t i = 0; i < blocks.size(); i++)
    {
        const bool left = i < leftCount;
        float& y = left ? leftY : rightY;
        for (const Line& line : blocks[i].lines)
        {
            float x = left ? margin : display.x - margin - ImGui::CalcTextSize(line.text.c_str()).x;
            draw->AddText(ImVec2(x, y), line.header ? headerColor : textColor, line.text.c_str());
            y += lineHeight;
        }
        y += lineHeight * 0.5f; // Gap before the next category.
    }
}

// The codebase's own entries. Each is evaluated only while the overlay is
// visible (see UpdateDebugDisplay), so none of this runs with F3 off.
void GUIController::RegisterBuiltinDebugDisplays()
{
    AddBuiltin<uint32_t>("Performance", "FPS", [] { return static_cast<uint32_t>(std::lround(fps)); });
    AddBuiltin<std::string>("Performance", "Frame Time", [] { return FormatMs(frameTime); });
    AddBuiltin<std::string>("Performance", "Render CPU", []
    {
        // Total, then where it goes: per-pass record time (hiding the near-zero
        // ones) and the chunk passes' cull / sort / draw split.
        std::string text = FormatMs(state->timings.renderCpuMs.load());
        if (state->renderEngine)
        {
            for (const auto& [passName, ms] : state->renderEngine->GetPassRecordTimes())
            {
                if (ms >= 0.05f) text += "\n\t" + passName + ": " + FormatMs(ms);
            }
        }
        char phases[96];
        std::snprintf(phases, sizeof(phases), "\n\tChunk cull / sort / draw: %.2f / %.2f / %.2f ms",
            static_cast<double>(state->chunkPhaseMs[GlobalState::CHUNK_PHASE_CULL]),
            static_cast<double>(state->chunkPhaseMs[GlobalState::CHUNK_PHASE_SORT]),
            static_cast<double>(state->chunkPhaseMs[GlobalState::CHUNK_PHASE_DRAW]));
        return text + phases;
    });
    AddBuiltin<std::string>("Performance", "Waiting on GPU", [] { return FormatMs(state->timings.gpuWaitMs.load()); });
    AddBuiltin<std::string>("Performance", "GPU Time", []
    {
        float gpuMs = state->timings.gpuMs.load();
        return gpuMs < 0.0f ? std::string("n/a") : FormatMs(gpuMs);
    });
    AddBuiltin<std::string>("Performance", "Mesher", []
    {
        return FormatMs(state->timings.meshMs.load()) + "/section, "
            + std::to_string(state->timings.meshQueue.load()) + " queued";
    });
    AddBuiltin<std::string>("Performance", "Present Mode", []
    {
        // GlobalState::presentMode is the swapchain's actual active
        // VkPresentModeKHR
        switch (static_cast<VkPresentModeKHR>(state->presentMode))
        {
            case VK_PRESENT_MODE_FIFO_KHR: return std::string("VSync (FIFO)");
            case VK_PRESENT_MODE_IMMEDIATE_KHR: return std::string("Immediate");
            case VK_PRESENT_MODE_MAILBOX_KHR: return std::string("Mailbox");
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return std::string("FIFO Relaxed");
            default: return std::string("Unknown");
        }
    });

    AddBuiltin<std::array<std::string, 5>>("Rendering", "Triangles", []
    {
        uint64_t total = 0;
        for (uint64_t count : state->trianglesDrawn) total += count;
        return std::array<std::string, 5>{
            std::to_string(total),
            "Terrain: " + std::to_string(state->trianglesDrawn[GlobalState::TRIANGLES_TERRAIN]),
            "Cutout: " + std::to_string(state->trianglesDrawn[GlobalState::TRIANGLES_CUTOUT]),
            "Water: " + std::to_string(state->trianglesDrawn[GlobalState::TRIANGLES_TRANSLUCENT]),
            "Entities: " + std::to_string(state->trianglesDrawn[GlobalState::TRIANGLES_ENTITY])
        };
    });
    AddBuiltin<std::string>("Rendering", "Sections", []
    {
        return std::to_string(state->sectionsTested - state->sectionsOccluded) + " drawn, "
            + std::to_string(state->sectionsOccluded) + " Hi-Z occluded";
    });

    AddBuiltin<std::string>("Memory", "RAM", [] { return ToMB(GetProcessMemoryUsageBytes()); });
    AddBuiltin<std::string>("Memory", "VRAM", []
    {
        VmaBudget heapBudgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(vmaAllocator, heapBudgets);
        uint64_t usage = 0;
        for (const VmaBudget& budget : heapBudgets) usage += budget.usage;
        return ToMB(usage);
    });
    AddBuiltin<std::string>("Memory", "Cubic Slab", []
    {
        SlabStats slab = ChunkMesher::GetSlabStats();
        return ToMB(slab.used) + " used / " + ToMB(slab.capacity) + " committed (limit " + ToMB(slab.limit) + ", " + std::to_string(slab.blocks) + " blocks)";
    });
    AddBuiltin<std::string>("Memory", "Non-Cubic Slab", []
    {
        SlabStats slab = NonCubicMesher::GetSlabStats();
        return ToMB(slab.used) + " used / " + ToMB(slab.capacity) + " committed (limit " + ToMB(slab.limit) + ", " + std::to_string(slab.blocks) + " blocks)";
    });

    AddBuiltin<std::string>("System", "CPU", []
    {
        return std::to_string(state->systemInfo.cpuLogicalCores) + "x " + state->systemInfo.cpuModel;
    });
    AddBuiltin<std::string>("System", "GPU", [] { return state->systemInfo.gpuModel; });
    AddBuiltin<std::string>("System", "Display", []
    {
        // Live framebuffer size, not GlobalState::resolution (the
        // startup-config value). Safe here: this runs on the GLFW window's thread.
        int width = 0, height = 0;
        glfwGetFramebufferSize(state->window, &width, &height);
        return std::to_string(width) + "x" + std::to_string(height) + " (" + state->systemInfo.gpuDriverVendor + ")";
    });
    AddBuiltin<std::string>("System", "Graphics API", []
    {
        const SystemInfo& sys = state->systemInfo;
        return "Vulkan " + std::to_string(VK_API_VERSION_MAJOR(sys.vulkanApiVersion)) + "."
            + std::to_string(VK_API_VERSION_MINOR(sys.vulkanApiVersion)) + "."
            + std::to_string(VK_API_VERSION_PATCH(sys.vulkanApiVersion));
    });

    // Network: what actually crossed the wire (see NetworkStats), to tell "the
    // server stopped sending" from "the client stopped handling it".
    AddBuiltin<std::string>("Network", "Traffic", [last = TrafficSample{}]() mutable
    {
        // Rates over the time since the HUD last evaluated this.
        auto now = std::chrono::steady_clock::now();
        uint64_t packetsIn = networkStats.packetsIn.load(), packetsOut = networkStats.packetsOut.load();
        uint64_t bytesIn = networkStats.bytesIn.load(), bytesOut = networkStats.bytesOut.load();
        double seconds = std::chrono::duration<double>(now - last.time).count();
        std::string text = "n/a";
        if (last.valid && seconds > 0.0)
        {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "in %.0f/s (%.1f KB/s), out %.0f/s (%.1f KB/s)",
                static_cast<double>(packetsIn - last.packetsIn) / seconds,
                static_cast<double>(bytesIn - last.bytesIn) / 1024.0 / seconds,
                static_cast<double>(packetsOut - last.packetsOut) / seconds,
                static_cast<double>(bytesOut - last.bytesOut) / 1024.0 / seconds);
            text = buffer;
        }
        last = TrafficSample{ true, now, packetsIn, packetsOut, bytesIn, bytesOut };
        return text;
    });
    AddBuiltin<std::string>("Network", "Last Packet", []
    {
        auto format = [](double ms) {
            if (ms < 0.0) return std::string("never");
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.0f ms ago", ms);
            return std::string(buffer);
        };
        return "in " + format(NetworkStats::AgeMs(networkStats.lastInTicks.load()))
            + ", out " + format(NetworkStats::AgeMs(networkStats.lastOutTicks.load()));
    });
    AddBuiltin<std::string>("Network", "Counters", []
    {
        auto in = [](int32_t id) { return networkStats.inById[static_cast<size_t>(id)].load(); };
        auto out = [](int32_t id) { return networkStats.outById[static_cast<size_t>(id)].load(); };
        std::string text = "since launch, in / out";
        text += "\n\tTeleports: " + std::to_string(in(PlayS2C::SynchronizePlayerPosition))
            + " received / " + std::to_string(out(PlayC2S::ConfirmTeleportation)) + " confirmed";
        text += "\n\tMovement: " + std::to_string(out(PlayC2S::SetPlayerPosition) + out(PlayC2S::SetPlayerPositionAndRotation))
            + " position, " + std::to_string(out(PlayC2S::SetPlayerRotation)) + " rotation, "
            + std::to_string(out(PlayC2S::SetPlayerMovementFlags)) + " flags";
        text += "\n\tTick ends: " + std::to_string(out(PlayC2S::ClientTickEnd));
        text += "\n\tBlocks: " + std::to_string(in(PlayS2C::BlockUpdate) + in(PlayS2C::MultiBlockChange))
            + " updates in / " + std::to_string(out(PlayC2S::BlockPlace)) + " places out";
        text += "\n\tEntities: " + std::to_string(in(PlayS2C::RelEntityMove) + in(PlayS2C::EntityMoveLook))
            + " moves + " + std::to_string(in(PlayS2C::EntityTeleport)) + " teleports in";
        text += "\n\tCombat: " + std::to_string(out(PlayC2S::Attack)) + " attacks / "
            + std::to_string(out(PlayC2S::UseEntity)) + " interacts out";
        // What the server sent back that an accepted hit would cause: if attacks
        // go up and none of these do, the server is dropping them.
        text += "\n\tServer replies: " + std::to_string(in(PlayS2C::DamageEvent)) + " damage, "
            + std::to_string(in(PlayS2C::HurtAnimation)) + " hurt, "
            + std::to_string(in(PlayS2C::SetEntityMotion)) + " motion, "
            + std::to_string(in(PlayS2C::EntityEvent)) + " entity events, "
            + std::to_string(in(PlayS2C::Animate)) + " animations";
        text += "\n\tHeld slot / loaded: " + std::to_string(out(PlayC2S::SetHeldItem)) + " slot reports, "
            + std::to_string(out(PlayC2S::PlayerLoaded)) + " player-loaded";
        text += "\n\tKeep-alive: " + std::to_string(in(PlayS2C::KeepAlive)) + " in / "
            + std::to_string(out(PlayC2S::KeepAlive)) + " out";
        return text;
    });
    // Keep-alive timing. If the server's own clock (id delta) advances far
    // less than the wall-clock gap between challenges, the server is running
    // slow or its packets are being held up.
    AddBuiltin<std::string>("Network", "Keep-alive", []
    {
        if (networkStats.keepAliveInTicks.load() == 0) return std::string("none received");
        char buffer[192];
        int64_t gapUs = networkStats.keepAliveGapUs.load();
        int64_t replyUs = networkStats.keepAliveReplyUs.load();
        std::string gap = "n/a";
        if (gapUs >= 0)
        {
            std::snprintf(buffer, sizeof(buffer), "%.0f ms (server clock +%lld ms)",
                static_cast<double>(gapUs) / 1000.0, static_cast<long long>(networkStats.keepAliveIdDelta.load()));
            gap = buffer;
        }
        std::snprintf(buffer, sizeof(buffer), "last %.1f s ago, replied in %.2f ms, gap since previous %s",
            NetworkStats::AgeMs(networkStats.keepAliveInTicks.load()) / 1000.0,
            replyUs >= 0 ? static_cast<double>(replyUs) / 1000.0 : 0.0, gap.c_str());
        return std::string(buffer);
    });
    AddBuiltin<std::string>("Network", "Entities", []
    {
        // How many the client tracks, and how stale the freshest server update
        // is — a frozen world shows as a large (or "never") age here.
        size_t count = 0;
        auto newest = std::chrono::steady_clock::time_point{};
        {
            std::lock_guard<std::mutex> lock(state->entitiesMutex);
            state->ForEachEntity([&](uint32_t, const Entity& entity)
            {
                count++;
                if (entity.lastUpdateTime > newest) newest = entity.lastUpdateTime;
            });
        }
        std::string age = "never";
        if (newest != std::chrono::steady_clock::time_point{})
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.0f ms ago",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - newest).count());
            age = buffer;
        }
        return std::to_string(count) + " tracked, newest update " + age;
    });

    AddBuiltin<std::string>("Player", "Position", []
    {
        glm::vec3 position = state->player->GetPosition();
        char text[96];
        std::snprintf(text, sizeof(text), "(%.5f, %.5f, %.5f)", static_cast<double>(position.x),
            static_cast<double>(position.y), static_cast<double>(position.z));
        return std::string(text);
    });
    AddBuiltin<std::string>("Player", "movement_speed", []
    {
        // What NetworkClient last stored from Update Attributes (or the default).
        double speed = state->attributes ? state->attributes->GetDouble("generic.movement_speed", -1.0) : -1.0;
        char text[32];
        std::snprintf(text, sizeof(text), "%.7g", speed);
        return std::string(text);
    });
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
        bool debugText = false;
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
    bool occlusionCulling = std::get<bool>(state->config->Get("Graphics.EntityOcclusionCulling", false));
    // Debug.EntityTags: every entity gets its type and coordinates. Otherwise only players are tagged, by name.
    bool debugTags = std::get<bool>(state->config->Get("Debug.EntityTags", false));

    // Cheap tests only; text is built afterward for the nearest few.
    auto consider = [&](const Entity& entity, const std::string* name)
    {
        if (!entity.visible) return;

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
        if (name == nullptr && occlusionCulling && occlusionSnapshot.IsEntityOccluded(feet, entity.boundingBox.x, entity.boundingBox.y)) return;

        candidates.push_back({ distanceSquared, &entity, name,
            ImVec2((ndcX * 0.5f + 0.5f) * display.x, (ndcY * 0.5f + 0.5f) * display.y) });
    };

    // We have no entity of our own; stand one in while the camera is off our head.
    PlayerEntity selfEntity;
    if (!state->player->camera.IsFirstPerson() && localId >= 0)
    {
        static const uint16_t playerType = []
        {
            for (uint16_t id = 0; id < 512; id++)
            {
                const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(id);
                if (info != nullptr && info->entityKind == "player") return id;
            }
            return uint16_t{0};
        }();

        selfEntity.id = static_cast<uint32_t>(localId);
        selfEntity.type = playerType;
        selfEntity.position = selfEntity.previousPosition = state->tickLoop->GetRenderPosition();
        {
            std::lock_guard<std::mutex> lock(state->localUsernameMutex);
            selfEntity.username = state->localUsername;
        }
        consider(selfEntity, &selfEntity.username);
    }

    {
        std::lock_guard<std::mutex> lock(state->entitiesMutex);
        for (const auto& [id, entity] : state->entities)
        {
            if (debugTags && static_cast<int32_t>(id) != localId) consider(entity, nullptr);
        }
        for (const auto& [id, player] : state->playerEntities)
        {
            if (static_cast<int32_t>(id) != localId) consider(player, &player.username);
        }

        if (candidates.size() > MAX_TAGS)
        {
            std::nth_element(candidates.begin(), candidates.begin() + MAX_TAGS, candidates.end(),
                [](const Candidate& a, const Candidate& b) { return a.distanceSquared < b.distanceSquared; });
            candidates.resize(MAX_TAGS);
        }

        for (const Candidate& candidate : candidates)
        {
            const Entity& entity = *candidate.entity;
            bool detailed = debugTags && candidate.distanceSquared <= DETAIL_DISTANCE_SQUARED;

            TagText& text = cache[entity.id];
            text.lastUsed = frame;
            if (!text.built || text.type != entity.type || text.debugText != debugTags || text.detailed != detailed
                || (detailed && text.shownPosition != entity.position) || text.fontSize != fontSize)
            {
                bool typeChanged = !text.built || text.type != entity.type || text.debugText != debugTags;
                text.debugText = debugTags;
                text.built = true;
                text.detailed = detailed;
                text.shownPosition = entity.position;
                text.fontSize = fontSize;
                text.type = entity.type;

                if (typeChanged)
                {
                    const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity.type);
                    if (debugTags)
                    {
                        text.lines[0] = info != nullptr ? info->name : "unknown";
                        if (candidate.name != nullptr && !candidate.name->empty()) text.lines[0] += " '" + *candidate.name + "'";
                    }
                    else
                    {
                        text.lines[0] = candidate.name != nullptr ? *candidate.name : std::string();
                    }
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

    // A screen (e.g. the connect screen) takes over the whole frame - it
    // hides every other window, including the debug HUD, while it's open.
    if (activeScreen != nullptr)
    {
        if (!IsDrawnByPincel(*activeScreen)) RenderScreen(*activeScreen);
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

            ImGui::SetNextWindowPos(ResolveAnchor(window->anchor, size, ImVec2(window->offsetX, window->offsetY)), ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            ImGui::Begin(window->name.c_str(), nullptr, flags);

            for (GUI::GUIComponent* component : window->components)
            {
                RenderComponent(component);
            }
            if (window->customContent) window->customContent();

            ImGui::End();
        }

        RenderDebugOverlay();
        if (state->input->IsActive("PlayerList")) RenderPlayerListWindow();
        RenderCrosshair();

        if (scene != nullptr && state->connectionPhase.load() == ConnectionPhase::Playing
            && std::get<bool>(state->config->Get("Graphics.Nametags", true)))
        {
            RenderNametags(*scene);
        }

        RenderHudElements();
        if (state->modLoader != nullptr) state->modLoader->Fire(EventType::HUD_RENDER);
    }

    ImGui::Render();
    AddTextShadows(ImGui::GetDrawData());
    ImGuiVulkan::RenderDrawData(imguiContext, commandBuffer);

    // Only a screen Update() has drawn: one opened since then waits a frame.
    if (activeScreen != nullptr && activeScreen == pincelScreen) controlRenderer->Record(commandBuffer);

    // The HUD (hotbar, status, chat) is Pincel's, over everything ImGui drew.
    if (activeScreen == nullptr && hudDrawn) hudRenderer->Record(commandBuffer);
}

// Sends what was typed in the chat box (a command if it starts with "/") and closes it.
void GUIController::SubmitChatInput(const std::string& text)
{
    Log::Info(std::string("Chat input submitted: '") + text + "'");
    if (!text.empty() && text[0] == '/') SendChatCommand(state, text.substr(1));
    else if (!text.empty()) SendChatMessage(state, text);
    CloseChatInput();
}

bool GUIController::IsDrawnByPincel(const GUI::Screen& screen)
{
    return controlRenderer != nullptr && GUI::ControlRenderer::Draws(screen);
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
                bool runDark = fontCmd && i + 3 <= cmd.ElemCount && IsDarkText(cmdVertices[indices[i]].col);
                while (fontCmd && runEnd + 3 <= cmd.ElemCount
                    && isText(cmdVertices[indices[runEnd]])
                    && isText(cmdVertices[indices[runEnd + 1]])
                    && isText(cmdVertices[indices[runEnd + 2]])
                    && IsDarkText(cmdVertices[indices[runEnd]].col) == runDark)
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

                if (!runDark && vertexCount + blockSize <= vertexCapacity)
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

    // Taller frames, so every control matches the standard control height.
    ControlStyle controlStyle;

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
}

// Draw the active Screen fullscreen, title first, then its components
// stacked and centered
void GUIController::RenderScreen(GUI::Screen& screen)
{
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    // The live display size (kept in sync with the real framebuffer every
    // frame), not the launch-time Window.Width/Height config - otherwise a
    // resize or F11 fullscreen leaves this covering only the old size.
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    // Keyboard/gamepad navigation only works in the focused window, so a
    // newly opened screen takes focus (its first control gets it below).
    static const GUI::Screen* lastScreen = nullptr;
    bool screenJustOpened = lastScreen != &screen;
    lastScreen = &screen;
    if (screenJustOpened) ImGui::SetNextWindowFocus();
    // No window background
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
    // that block measured last frame (ImGui only knows it after drawing) -
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
    // needs its own layout
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

// Vanilla-style Tab-list overlay
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

// Hotbar/armor/status-bar/movement-state geometry
namespace {
    // Vanilla GUI geometry in GUI units: 1 unit = 1 texture pixel, and
    // Graphics.GUIScale scales the whole thing (see ApplyGUIScale).
    constexpr float ITEM_SIZE = 16.0f;          // Item icon area inside a slot.
    constexpr float HOTBAR_WIDTH = 182.0f;
    constexpr float HOTBAR_HEIGHT = 22.0f;
    constexpr float HUD_BOTTOM_MARGIN = 2.0f; // hotbar's own inset from the bottom edge

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
            const float COUNT_FONT_SIZE = Art(9.0f);
            char text[8];
            std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(count));
            ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(COUNT_FONT_SIZE, FLT_MAX, 0.0f, text);
            ImVec2 textPos(pos.x + Art(ITEM_SIZE + 1.0f) - textSize.x, pos.y + Art(ITEM_SIZE + 1.0f) - Art(8.0f));
            drawList->AddText(ImGui::GetFont(), COUNT_FONT_SIZE, ImVec2(textPos.x + Art(1.0f), textPos.y + Art(1.0f)),
                IM_COL32(63, 63, 63, 255), text);
            drawList->AddText(ImGui::GetFont(), COUNT_FONT_SIZE, textPos, IM_COL32(255, 255, 255, 255), text);
        }
    }
}

namespace {
    struct HudElement {
        std::string name;
        std::function<void()> draw;
    };
    std::mutex hudElementsMutex; // Guards hudElements; held only to edit or copy the list.
    std::vector<HudElement> hudElements;
}

void GUIController::AddHudElement(const std::string& name, std::function<void()> draw)
{
    std::lock_guard<std::mutex> lock(hudElementsMutex);
    hudElements.erase(std::remove_if(hudElements.begin(), hudElements.end(),
        [&](const HudElement& element) { return element.name == name; }), hudElements.end());
    if (draw) hudElements.push_back(HudElement{ name, std::move(draw) });
}

void GUIController::RemoveHudElement(const std::string& name)
{
    AddHudElement(name, nullptr);
}

void GUIController::RenderHudElements()
{
    std::vector<HudElement> elements;
    {
        std::lock_guard<std::mutex> lock(hudElementsMutex);
        elements = hudElements;
    }
    for (const HudElement& element : elements) element.draw();
}

void GUIController::DrawItem(ImDrawList* drawList, ImVec2 pos, const ItemStack& item)
{
    DrawItemIcon(drawList, pos, IconForItem(item), item.count);
}

ImTextureID GUIController::GetItemIconTexture(const ItemStack& item)
{
    return IconForItem(item);
}

float GUIController::GetArtScale()
{
    return g_artScale;
}

ImVec2 GUIController::GetHotbarOrigin()
{
    return HotbarOrigin();
}

// Top-left of the hotbar
ImVec2 GUIController::HotbarOrigin()
{
    return ResolveAnchor(GUI::ScreenAnchor::BOTTOM, ImVec2(Art(HOTBAR_WIDTH), Art(HOTBAR_HEIGHT)), ImVec2(0.0f, Art(HUD_BOTTOM_MARGIN)));
}

// Dark navy panels with a orange border tint, laid on top of StyleColorsDark.
// Called once at Init
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
float GUIController::GetControlHeight()
{
    ControlStyle controlStyle;
    return ImGui::GetFrameHeight();
}

bool GUIController::DrawButton(const std::string& label, float width)
{
    ControlStyle controlStyle;
    return DrawStyledButton(label, ImVec2(width, ImGui::GetFrameHeight()));
}

bool GUIController::DrawMenuButton(const std::string& label, float width)
{
    if (width <= 0.0f) width = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + 32.0f;
    CenterNextControl(width);
    return DrawButton(label, width);
}

bool GUIController::DrawInput(const std::string& id, char* buffer, size_t capacity, float width, const char* hint)
{
    ControlStyle controlStyle;
    ImGui::SetNextItemWidth(width > 0.0f ? width : ImGui::GetContentRegionAvail().x);
    std::string widgetId = id.rfind("##", 0) == 0 ? id : "##" + id;
    return ImGui::InputTextWithHint(widgetId.c_str(), hint != nullptr ? hint : "", buffer, capacity,
        ImGuiInputTextFlags_EnterReturnsTrue);
}

bool GUIController::DrawInput(const std::string& id, std::string& value, float width, const char* hint)
{
    // ImGui edits a fixed buffer; the text is copied out every frame, so `value`
    // always holds what is shown.
    constexpr size_t CAPACITY = 512;
    char buffer[CAPACITY];
    std::snprintf(buffer, CAPACITY, "%s", value.c_str());
    bool submitted = DrawInput(id, buffer, CAPACITY, width, hint);
    value = buffer;
    return submitted;
}

bool GUIController::DrawDropdown(const std::string& id, const std::vector<std::string>& options, size_t& selected,
                                 float width)
{
    ControlStyle controlStyle;
    ImGui::SetNextItemWidth(width > 0.0f ? width : ImGui::GetContentRegionAvail().x);
    std::string widgetId = id.rfind("##", 0) == 0 ? id : "##" + id;
    const char* preview = selected < options.size() ? options[selected].c_str() : "";

    bool changed = false;
    if (ImGui::BeginCombo(widgetId.c_str(), preview, ImGuiComboFlags_HeightLarge))
    {
        for (size_t i = 0; i < options.size(); i++)
        {
            bool isSelected = i == selected;
            if (ImGui::Selectable((options[i] + "##" + std::to_string(i)).c_str(), isSelected) && !isSelected)
            {
                selected = i;
                changed = true;
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void GUIController::CenterNextControl(float width)
{
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
}

void GUIController::AlignTextToControl()
{
    ControlStyle controlStyle;
    ImGui::AlignTextToFramePadding();
}

bool GUIController::DrawStyledButton(const std::string& label, ImVec2 size)
{
    if (size.x <= 0.0f) size.x = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + 32.0f;
    if (size.y <= 0.0f) size.y = GetControlHeight();

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
// an absolute position
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
// against the current ImGui display size
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
// main (window-owning) thread
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
// gives for doing so
void GUIController::OpenChatInput(const char* initialText)
{
    chatInputOpen = true;
    chatInputJustOpened = true;
    std::snprintf(chatInputBuffer, sizeof(chatInputBuffer), "%s", initialText);
    if (windowHandle != nullptr)
    {
        state->pendingCursorMode.store(GLFW_CURSOR_NORMAL);
    }
}

// Close the chat input box and re-capture the cursor for the fly-cam -
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
