#pragma once
#ifndef GUI_CONTROLLER_H
#define GUI_CONTROLLER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <imgui.h>
#include "../VulkanInit.hpp"
#include "../TextureManager.hpp"
#include "ImGuiVulkan.hpp"
#include "GlobalState.hpp"
#include "models/GUIWindow.hpp"
#include "models/Screen.hpp"

namespace Volcano {

class GUIController {
public:
    static void Init(GLFWwindow* window, VkRenderPass renderPass, uint32_t imageCount, GlobalState* globalState,
                      const TextureManager* textureManager);
    static void Shutdown();
    static void NewFrame();
    static void Update(float deltaTime);
    static void Render(VkCommandBuffer commandBuffer);

    static float GetFPS() { return fps; }
    static float GetFrameTime() { return frameTime; }

    // API:
    // Render a window, pinned to a screen anchor plus a pixel offset from it
    // (see GUI::ScreenAnchor) rather than an absolute position — so it stays
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
    static const TextureManager* textureManager;
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
    static GUI::Screen* deathScreen;
    static GUI::GUIComponent* deathMessageText;
    static GUI::Screen* inventoryScreen; // See RenderInventoryScreen's own comment.

    static bool chatInputOpen;
    static bool chatInputJustOpened; // Consumed once by RenderChatInputBox to grab keyboard focus the frame it opens.
    static char chatInputBuffer[256];

    // One ImGui-registered texture per distinct item icon actually drawn
    // this session, keyed by TextureManager texture-array layer (not by
    // item id — several items can share a layer, e.g. none currently do,
    // but nothing stops it) — created lazily the first time a layer is
    // needed and kept for the rest of the session rather than recreated
    // every frame every slot is drawn. See GetItemIcon()/Shutdown().
    struct ItemIcon {
        VkImageView view = VK_NULL_HANDLE; // A single-layer 2D view into TextureManager::array's underlying image.
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE; // == the ImTextureID ImGui draws with.
    };
    static std::unordered_map<uint16_t, ItemIcon> itemIconCache;

    // A single-image (not texture-array-layer) icon loaded straight from a
    // PNG on disk — for standalone HUD art like the crosshair that isn't
    // part of TextureManager's block/item atlas (wrong size, not a game
    // block/item). Owns the whole image, unlike ItemIcon above (which only
    // owns a view into TextureManager's shared image) — Shutdown() must
    // destroy the image/allocation too, not just the view.
    struct StandaloneIcon {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    };
    // Loaded once in Init(); null (every field VK_NULL_HANDLE) if the PNG is
    // missing or fails to load — RenderCrosshair() draws nothing in that
    // case rather than crashing startup over missing HUD art.
    static StandaloneIcon crosshairIcon;
    static StandaloneIcon crosshairEntityIcon; // "Looking at an entity" variant — see GlobalState::lookingAtEntity.

    // Loads one standalone PNG (path) into a StandaloneIcon — image, view,
    // and an ImGui-registered descriptor set, uploaded via the same
    // staging-buffer-then-copy path TextureManager::UploadLayer uses (see
    // BeginOneShotCommands/TransitionImageLayout in VulkanInit.hpp). Never
    // throws: any failure (missing file, Vulkan call failing) is logged and
    // returns a default (all-null) StandaloneIcon — see its own comment.
    static StandaloneIcon LoadStandaloneIcon(const std::string& path);
    // Draws the centered crosshair every HUD frame — plain or the
    // "looking at an entity" variant, per GlobalState::lookingAtEntity.
    static void RenderCrosshair();

    // Resolves (creating and caching on first use) the ImTextureID for one
    // texture-array layer, for drawing an item icon via ImDrawList::
    // AddImage. ImGui's Vulkan backend draws a single flat 2D texture per
    // ImTextureID, not an indexed array layer the way the 3D world pass
    // samples TextureManager::array — so each distinct layer needs its own
    // narrow (single-layer, single-mip) VkImageView and a descriptor set
    // registered via ImGui_ImplVulkan_AddTexture wrapping it.
    static ImTextureID GetItemIcon(uint16_t textureLayer);

    // Resolves an inventory slot's contents straight to a drawable icon —
    // ItemRegistry::Lookup + GetItemIcon in one call, so every slot-drawing
    // call site (hotbar/armor, the inventory screen) shares one path
    // instead of each re-deriving "empty slot / unresolved id / no icon
    // texture" separately. Returns 0 (draw nothing) for an empty slot, an
    // unknown item id, or an item with no resolved icon texture (see
    // ItemRegistry::ItemTypeInfo::hasTexture).
    static ImTextureID IconForItem(const ItemStack& item);

    static void RenderComponent(GUI::GUIComponent* component);
    static void RenderScreen(GUI::Screen& screen);
    static void RenderChatWindow();
    static void RenderChatInputBox();
    static void RenderPlayerListWindow();
    static void ApplyVolcanoTheme();

    // HUD elements drawn only while no Screen is open (see Render()) — the
    // hotbar/armor/status-bar/movement-state group all key off the same
    // hotbar geometry (HotbarOrigin), which is why they're separate methods
    // but share file-scope layout constants in the .cpp rather than each
    // reinventing "where's the hotbar" independently.
    static void RenderPlayerStatusBars();
    static void RenderHotbarAndArmor();
    static void RenderMovementStatePanel();

    // The player's own inventory screen (armor + main inventory grid +
    // hotbar), toggled by "E" (see Update()) the same way "Esc" toggles
    // pauseScreen — set as inventoryScreen's Screen::customContent, so it
    // gets OpenScreen/CloseScreen's usual HUD-hiding/cursor-release/
    // Escape-to-close behavior for free. View-only: there's no Click
    // Container Slot handling (or any other block/container interaction in
    // this client yet), so slots aren't clickable — this just shows
    // whatever NetworkClient's Set Container Content/Slot handlers have
    // written into state->inventory.
    static void RenderInventoryScreen();
    // Top-left corner of the (still-placeholder) hotbar, resolved fresh
    // every frame the same way every other HUD element is — see
    // ResolveAnchor. The other three HUD methods above all derive their own
    // position from this one point so they stay lined up with each other
    // even if the hotbar's own anchor/margin ever changes.
    static ImVec2 HotbarOrigin();

    // Resolves an anchor + inward pixel offset into an absolute top-left
    // window position, against the *current* ImGui display size (which
    // ImGui_ImplGlfw keeps in sync with the real framebuffer every frame —
    // unlike the launch-time Window.Width/Height config values, this is
    // correct immediately after a resize or an F11 window-mode change).
    static ImVec2 ResolveAnchor(GUI::ScreenAnchor anchor, ImVec2 size, ImVec2 offset);
};

} // namespace Volcano

#endif
