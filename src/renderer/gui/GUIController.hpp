#pragma once
#ifndef GUI_CONTROLLER_H
#define GUI_CONTROLLER_H

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>
#include <imgui.h>
#include "../VulkanInit.hpp"
#include "../TextureManager.hpp"
#include "ImGuiVulkan.hpp"
#include "ControlRenderer.hpp"
#include "HudRenderer.hpp"
#include "PincelSprite.hpp"
#include "GlobalState.hpp"
#include "models/GUIWindow.hpp"
#include "models/Screen.hpp"

namespace Volcano {

namespace Engine { struct SceneView; }

namespace DebugDisplayDetail {

template <typename T>
struct IsStringArray : std::false_type {};
template <size_t N>
struct IsStringArray<std::array<std::string, N>> : std::true_type {};

// Turns one debug value into display text. Numbers print plainly (floats to 2
// decimals, doubles to 4), a string is shown as-is, and an array of strings
// is one value spread over several lines (joined with "\n\t").
template <typename T>
std::string Format(const T& value)
{
    if constexpr (std::is_same_v<T, std::string>)
    {
        return value;
    }
    else if constexpr (IsStringArray<T>::value)
    {
        std::string joined;
        for (size_t i = 0; i < value.size(); i++)
        {
            if (i > 0) joined += "\n\t";
            joined += value[i];
        }
        return joined;
    }
    else if constexpr (std::is_same_v<T, float>)
    {
        char text[48];
        std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(value));
        return text;
    }
    else if constexpr (std::is_same_v<T, double>)
    {
        char text[48];
        std::snprintf(text, sizeof(text), "%.4f", value);
        return text;
    }
    else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> || std::is_same_v<T, uint32_t>
        || std::is_same_v<T, uint64_t> || std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t>)
    {
        return std::to_string(value);
    }
    else
    {
        static_assert(!sizeof(T*), "SetDebugDisplay: T must be uint8_t, uint16_t, uint32_t, uint64_t, int32_t, "
            "int64_t, float, double, std::string or std::array<std::string, N>");
    }
}

} // namespace DebugDisplayDetail

class GUIController {
public:
    static void Init(GLFWwindow* window, VkRenderPass renderPass, uint32_t imageCount, GlobalState* globalState,
                      const TextureManager* textureManager);
    static void Shutdown();
    // Rebuilds every texture-derived icon (standalone HUD art, item icons, 3D
    // model icons) after a resource reload, keeping the ImGui backend alive.
    // GPU must be idle, no ImGui frame open.
    static void ReloadTextures(const TextureManager* textureManager);
    static void NewFrame();
    static void Update(float deltaTime);
    // `scene` (the frame's camera) is what world-space HUD elements such as
    // nametags project through; null skips them.
    static void Render(VkCommandBuffer commandBuffer, const Engine::SceneView* scene = nullptr);
    // Ends the current ImGui frame without drawing (for a skipped frame).
    static void DiscardFrame();

    static float GetFPS() { return fps; }
    static float GetGUIScale() { return guiScale; }
    static float GetFrameTime() { return frameTime; }

    // Debug display API (the F3 overlay) - usable by the codebase and mods alike.
    //
    // Shows "<name>: <value>" under `category`. Categories are sorted
    // alphabetically (case-insensitive), as are the names inside each, and are
    // spread over the left and right sides of the screen, filling the left
    // first. `*update` is called only while the overlay is visible, a couple of
    // times a second, on the render thread, and returns the current value. T is
    // one of uint8_t, uint16_t, uint32_t, uint64_t, int32_t, int64_t, float,
    // double, std::string, or std::array<std::string, N> (N lines).
    //
    // A string value may hold several lines: separate them with "\n\t" (tabs
    // are drawn as 4 spaces), e.g. "3.4 ms\n\tTerrain: 1.2 ms\n\tGUI: 0.4 ms".
    //
    // Registering the same category and name again replaces the entry. The
    // pointed-to std::function must outlive the entry: the caller owns it, so
    // a mod must remove its entries (RemoveDebugDisplay, or SetDebugDisplay
    // with a null pointer) before the function is destroyed. `update` must not
    // call back into these functions.
    template <typename T>
    static void SetDebugDisplay(const std::string& category, const std::string& name, std::function<T()>* update)
    {
        if (update == nullptr)
        {
            RemoveDebugDisplay(category, name);
            return;
        }
        RegisterDebugEntry(category, name, [update]() { return DebugDisplayDetail::Format((*update)()); });
    }
    static void RemoveDebugDisplay(const std::string& category, const std::string& name);

    // API:
    // Render a window, pinned to a screen anchor plus a pixel offset from it
    // (see GUI::ScreenAnchor) rather than an absolute position - so it stays
    // correctly placed across resizes and window-mode changes.
    static GUI::GUIWindow* CreateWindow(const std::string& name, GUI::ScreenAnchor anchor,
                                         float offsetX = 8.0f, float offsetY = 8.0f);

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
    static GUI::Screen* GetActiveScreen() { return activeScreen; }
    // The screen Esc opens while in a world. Built by the caller (see
    // screens/PauseScreen) since its buttons need the connection.
    static void SetPauseScreen(GUI::Screen* screen) { pauseScreen = screen; }
    static GUI::Screen* GetPauseScreen() { return pauseScreen; }

    // Chat input box - "T" (see VolcanoClient.cpp's "OpenChat" action)
    // raises it; Enter sends the typed text (via NetworkClient::
    // SendChatMessage, a no-op if there's no live connection) and closes
    // it, Escape closes it without sending. Unlike a Screen this doesn't
    // hide the HUD or the chat scrollback (which shifts up to make room
    // for the input box instead) - it only needs to steal keyboard focus
    // and stop the mouse-look/movement inputs RenderThread::PollInputs
    // would otherwise also apply to whatever's being typed.
    static bool IsChatInputOpen() { return chatInputOpen; }
    // `initialText` pre-fills the box (e.g. "/" to start a command).
    static void OpenChatInput(const char* initialText = "");
    static void CloseChatInput();

    // Controls API - usable by the codebase and mods alike.
    //
    // The three standard controls, drawn the way the main menu draws its own:
    // one height (GetControlHeight), one look. Call them anywhere inside an
    // ImGui window (a HUD element, a Screen's customContent...); they work out
    // their own padding, so no layout math is needed at the call site. Widths
    // default to the main menu's (CONTROL_WIDTH); width <= 0 fits the label.
    // Place them like any ImGui item (SameLine, SetCursorPosX), or use
    // CenterNextControl / DrawMenuButton for the common centered column.
    //
    // `label` identifies the control: it must be unique within the window (add
    // "##something" after the visible text to tell apart two with the same text).
    static constexpr float CONTROL_WIDTH = 240.0f;

    // Height of every standard control, in ImGui units.
    static float GetControlHeight();

    // A button. Returns true on the frame it is clicked (or activated by keyboard).
    static bool DrawButton(const std::string& label, float width = CONTROL_WIDTH);

    // A button centered in the window: the main menu's column of buttons.
    static bool DrawMenuButton(const std::string& label, float width = CONTROL_WIDTH);

    // A single-line text box. `id` is hidden (it names the control, not a caption);
    // `hint` is the grey text shown while empty. The text is edited in place.
    // Returns true on the frame Enter is pressed inside it.
    static bool DrawInput(const std::string& id, std::string& value, float width = CONTROL_WIDTH,
                          const char* hint = nullptr);
    static bool DrawInput(const std::string& id, char* buffer, size_t capacity, float width = CONTROL_WIDTH,
                          const char* hint = nullptr);

    // A dropdown listing `options`, showing the selected one. `selected` is
    // updated; returns true on the frame the choice changes.
    static bool DrawDropdown(const std::string& id, const std::vector<std::string>& options, size_t& selected,
                             float width = CONTROL_WIDTH);

    // Moves the cursor so the next control of `width` is centered in the window.
    static void CenterNextControl(float width = CONTROL_WIDTH);

    // Call before a text label that sits beside a control (then SameLine), so
    // the text lines up vertically with the taller control.
    static void AlignTextToControl();

    // The raw styled button the controls above are built on: a flat-glow button
    // in the Volcano theme (filled rect + border, brighter on hover/press).
    // size.x/y <= 0 auto-sizes that axis (height = GetControlHeight()). Prefer
    // DrawButton, which also handles the control padding.
    static bool DrawStyledButton(const std::string& label, ImVec2 size = ImVec2(0.0f, 0.0f));

    // HUD API - usable by the codebase and mods alike.
    //
    // Adds a HUD element: `draw` runs every frame on the render thread, after
    // the vanilla HUD and inside the open ImGui frame, while no Screen is open.
    // Call any ImGui function in it (its own Begin/End window, or
    // ImGui::GetBackgroundDrawList()). Adding a name again replaces the
    // element. The HUD_RENDER mod event fires right after these run.
    static void AddHudElement(const std::string& name, std::function<void()> draw);
    static void RemoveHudElement(const std::string& name);

    // Building blocks for HUD widgets, the same ones the hotbar uses.
    //
    // Draws an item's icon, and its count when above 1, with the top-left of the
    // 16x16 item area at `pos`. An empty stack draws nothing. Pair with
    // InventoryManager::TakeSnapshot() to read the inventory without locking.
    static void DrawItem(ImDrawList* drawList, ImVec2 pos, const ItemStack& item);
    // The icon texture alone (0 when there is none), for custom drawing.
    static ImTextureID GetItemIconTexture(const ItemStack& item);
    // The item's icon as pixels in memory, for Pincel: its rendered 3D model, or its flat sprite.
    // Null for an empty stack or an item with no icon. Cached; render thread only.
    static const GUI::Sprite* GetItemSprite(const ItemStack& item);
    // Pixels per vanilla texture pixel; multiply vanilla-sized geometry by it
    // (a slot is 18 units, an item 16).
    static float GetArtScale();
    // Top-left of the hotbar on screen: anchor widgets to it to avoid overlap.
    static ImVec2 GetHotbarOrigin();

private:
    static void RenderHudElements();
    static GlobalState* state;
    static GLFWwindow* windowHandle;
    static const TextureManager* textureManager;
    static std::vector<GUI::GUIWindow*> windows;
    static GUI::Screen* activeScreen;
    // Draws screens made of plain components with Pincel; null if Pincel didn't start.
    static std::unique_ptr<GUI::ControlRenderer> controlRenderer;
    // The screen the last Update() drew with it, so Render() only lays down a texture that is current.
    static const GUI::Screen* pincelScreen;
    static bool IsDrawnByPincel(const GUI::Screen& screen);
    // Draws the in-game HUD (hotbar, status, chat) with Pincel; null if Pincel didn't start.
    static std::unique_ptr<GUI::HudRenderer> hudRenderer;
    static bool hudDrawn; // Whether Update() drew it, so Render() only lays down a current texture.
    static ImGuiVulkanContext imguiContext;
    static bool initialized;
    static float fps;
    // Current GUI scale (see ApplyGUIScale) and how many queued ImGui input
    // events were already scaled for it.
    static inline float guiScale = 1.0f;
    static inline int scaledInputEvents = 0;
    static void ApplyGUIScale();
    static void AddTextShadows(ImDrawData* drawData);
    static float frameTime;
    static int frameCount;
    static float fpsTimer;
    // Debug display internals - see SetDebugDisplay. Entries live in the .cpp.
    static void RegisterDebugEntry(const std::string& category, const std::string& name, std::function<std::string()> format);
    static void RegisterBuiltinDebugDisplays();
    static void UpdateDebugDisplay(float deltaTime);
    static void RenderDebugOverlay();
    static GUI::Screen* pauseScreen;
    static GUI::Screen* deathScreen;
    static GUI::GUIComponent* deathMessageText;
    static GUI::Screen* inventoryScreen; // See screens/InventoryScreen.

    static bool chatInputOpen;
    static bool chatInputJustOpened; // Consumed once by the HUD to focus the box the frame it opens.
    static char chatInputBuffer[256];

    // One ImGui-registered texture per distinct item icon actually drawn
    // this session, keyed by TextureManager texture-array layer (not by
    // item id - several items can share a layer, e.g. none currently do,
    // but nothing stops it) - created lazily the first time a layer is
    // needed and kept for the rest of the session rather than recreated
    // every frame every slot is drawn. See GetItemIcon()/Shutdown().
    struct ItemIcon {
        VkImageView view = VK_NULL_HANDLE; // A single-layer 2D view into TextureManager::array's underlying image.
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE; // == the ImTextureID ImGui draws with.
    };
    static std::unordered_map<uint16_t, ItemIcon> itemIconCache;

    // A single-image (not texture-array-layer) icon loaded straight from a
    // PNG on disk - for standalone HUD art like the crosshair that isn't
    // part of TextureManager's block/item atlas (wrong size, not a game
    // block/item). Owns the whole image, unlike ItemIcon above (which only
    // owns a view into TextureManager's shared image) - Shutdown() must
    // destroy the image/allocation too, not just the view.
    struct StandaloneIcon {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    };
    // Loaded once in Init(); null (every field VK_NULL_HANDLE) if the PNG is
    // missing or fails to load - RenderCrosshair() draws nothing in that
    // case rather than crashing startup over missing HUD art.
    static StandaloneIcon crosshairIcon;
    static StandaloneIcon crosshairEntityIcon; // "Looking at an entity" variant - see GlobalState::lookingAtEntity.

    // Vanilla GUI art (textures/gui/...). Same load/failure behavior as the
    // crosshair: null if missing, and callers fall back to a flat rect.
    static StandaloneIcon attackIndicatorBackgroundIcon; // sprites/hud/crosshair_attack_indicator_background.png (16x4)
    static StandaloneIcon attackIndicatorProgressIcon;   // sprites/hud/crosshair_attack_indicator_progress.png (16x4)
    static StandaloneIcon hotbarIcon;              // sprites/hud/hotbar.png (182x22)
    static StandaloneIcon hotbarSelectionIcon;      // sprites/hud/hotbar_selection.png (24x23)
    static StandaloneIcon slotHighlightBackIcon;    // sprites/container/slot_highlight_back.png (24x24)
    static StandaloneIcon slotHighlightFrontIcon;   // sprites/container/slot_highlight_front.png (24x24)
    static StandaloneIcon inventoryBackground;      // container/inventory.png (256x256 sheet, 176x166 used)
    static StandaloneIcon craftingBackground;       // container/crafting_table.png
    static StandaloneIcon furnaceBackground;        // container/furnace.png
    static StandaloneIcon genericBackground;        // container/generic_54.png (chest-style grids)
    static StandaloneIcon logoIcon;                 // resources/logo.png, drawn 128x128 on screens with Screen::showLogo
    static StandaloneIcon heartContainerIcon;       // sprites/hud/heart/container.png (9x9)
    static StandaloneIcon heartFullIcon;
    static StandaloneIcon heartHalfIcon;
    static StandaloneIcon foodEmptyIcon;            // sprites/hud/food_empty.png (9x9)
    static StandaloneIcon foodFullIcon;
    static StandaloneIcon foodHalfIcon;
    static StandaloneIcon saturationOutlineIcon;    // food_empty's outline recolored gold (AppleSkin-style saturation)
    static StandaloneIcon experienceBackgroundIcon; // sprites/hud/experience_bar_background.png (182x5)
    static StandaloneIcon experienceProgressIcon;   // sprites/hud/experience_bar_progress.png (182x5)
    static StandaloneIcon armorEmptyIcon;
    static StandaloneIcon armorFullIcon;
    static StandaloneIcon armorHalfIcon;

    // ImGui texture handle for a StandaloneIcon, or 0 if it failed to load.
    static ImTextureID TextureOf(const StandaloneIcon& icon);
    static void LoadStandaloneIcons();
    static void FreeIcons(bool removeDescriptors);

    // Per-item 3D icons (blocks and custom models), rendered once by
    // ItemRenderer. `resolved` means the lookup ran; `icon` is null when the
    // item should use its flat 2D sprite instead. Keyed by item id.
    struct ModelIcon {
        bool resolved = false;
        StandaloneIcon icon;
    };
    static std::unordered_map<uint16_t, ModelIcon> modelIconCache;
    static std::unordered_map<uint16_t, GUI::Sprite> itemSprites; // Empty sprite = no icon.
    static ImTextureID ModelIconForItem(uint16_t itemId, const std::string& itemName);

    // Loads one standalone PNG (path) into a StandaloneIcon - image, view,
    // and an ImGui-registered descriptor set, uploaded via the same
    // staging-buffer-then-copy path TextureManager::UploadLayer uses (see
    // BeginOneShotCommands/TransitionImageLayout in VulkanInit.hpp). Never
    // throws: any failure (missing file, Vulkan call failing) is logged and
    // returns a default (all-null) StandaloneIcon - see its own comment.
    // A non-zero outlineColor (IM_COL32) keeps only the sprite's black
    // outline pixels, recolored, and clears the rest.
    static StandaloneIcon LoadStandaloneIcon(const std::string& path, uint32_t outlineColor = 0);
    // Draws the centered crosshair every HUD frame - plain or the
    // "looking at an entity" variant, per GlobalState::lookingAtEntity.
    static void RenderCrosshair();
    // Debug nametags above every entity: type, name, position. Plain text, so
    // AddTextShadows gives it the UI's shadow. Graphics.Nametags toggles it.
    static void RenderNametags(const Engine::SceneView& scene);
    // Bar under the crosshair: block-break progress or attack recharge.
    static void RenderAttackIndicator(const ImVec2& center);

    // Resolves (creating and caching on first use) the ImTextureID for one
    // texture-array layer, for drawing an item icon via ImDrawList::
    // AddImage. ImGui's Vulkan backend draws a single flat 2D texture per
    // ImTextureID, not an indexed array layer the way the 3D world pass
    // samples TextureManager::array - so each distinct layer needs its own
    // narrow (single-layer, single-mip) VkImageView and a descriptor set
    // registered via ImGui_ImplVulkan_AddTexture wrapping it.
    static ImTextureID GetItemIcon(uint16_t textureLayer);

    // Resolves an inventory slot's contents straight to a drawable icon -
    // ItemRegistry::Lookup + GetItemIcon in one call, so every slot-drawing
    // call site (hotbar/armor, the inventory screen) shares one path
    // instead of each re-deriving "empty slot / unresolved id / no icon
    // texture" separately. Returns 0 (draw nothing) for an empty slot, an
    // unknown item id, or an item with no resolved icon texture (see
    // ItemRegistry::ItemTypeInfo::hasTexture).
    static ImTextureID IconForItem(const ItemStack& item);

    static void RenderComponent(GUI::GUIComponent* component, float width = 240.0f, float labelWidth = 0.0f);
    // Sliders, option buttons and number fields: drawn as "label | control" rows on screens.
    static bool IsLabeledControl(GUI::GUIComponent* component);
    static void DrawSideTooltip(const std::string& text, ImVec2 itemMin, ImVec2 itemMax, bool onLeft);
    static void RenderScreen(GUI::Screen& screen);
    static void SubmitChatInput(const std::string& text);
    static void RenderPlayerListWindow();
    static void ApplyVolcanoTheme();

    static void CloseInventoryWindow();

    // Top-left corner of the (still-placeholder) hotbar, resolved fresh
    // every frame the same way every other HUD element is - see
    // ResolveAnchor. The other three HUD methods above all derive their own
    // position from this one point so they stay lined up with each other
    // even if the hotbar's own anchor/margin ever changes.
    static ImVec2 HotbarOrigin();

    // Resolves an anchor + inward pixel offset into an absolute top-left
    // window position, against the *current* ImGui display size (which
    // ImGui_ImplGlfw keeps in sync with the real framebuffer every frame -
    // unlike the launch-time Window.Width/Height config values, this is
    // correct immediately after a resize or an F11 window-mode change).
    static ImVec2 ResolveAnchor(GUI::ScreenAnchor anchor, ImVec2 size, ImVec2 offset);
};

} // namespace Volcano

#endif
