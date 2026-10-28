#include "InventoryScreen.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <imgui.h>
#include "../ControlRenderer.hpp"
#include "../GUIController.hpp"
#include "../ItemSlotView.hpp"
#include "../PincelSprite.hpp"
#include "../Theme.hpp"
#include "DropKey.hpp"
#include "InputHandler.hpp"
#include "inventory/ItemRegistry.hpp"
#include "inventory/ToolStats.hpp"

// The network side, declared here rather than including NetworkClient.hpp (see GUIController.cpp for why).
namespace Volcano {
void SendInventoryClick(GlobalState* state, int32_t windowId, int32_t stateId, int32_t slot,
                        uint8_t button, const ItemStack& slotItem, const ItemStack& carriedItem);
void SendInventoryAction(GlobalState* state, int32_t windowId, int32_t stateId, int32_t slot,
                         uint8_t button, int32_t mode, const ItemStack& carriedItem);
}

namespace Volcano::GUI {

namespace {

using Pincel::Color;
using Pincel::Element;
using Pincel::Orientation;
using Pincel::PointerEvents;
using Pincel::Position;

constexpr float ITEM_SIZE = 16.0f;
constexpr float PANEL_WIDTH = 176.0f;
constexpr int HOTBAR_SLOTS = 9;
constexpr const char* ART = "assets/minecraft/textures/gui/";

constexpr Color LABEL_COLOR{ 64, 64, 64 }; // Vanilla's, on the light panel.
constexpr Color NO_SHADOW{ 0, 0, 0, 0.0f }; // Dark text gets no shadow, which would only muddy it.

// Where each slot of the open window goes, in texture pixels: the top-left of the 16x16 item area,
// relative to the panel (its art already has the slot frames drawn).
struct SlotDef {
    int32_t index; // The protocol slot index.
    float x;
    float y;
};

struct Layout {
    std::vector<SlotDef> slots;
    std::string background = std::string(ART) + "container/inventory.png";
    float panelHeight = 166.0f;
    int genericRows = 0; // Above 0: generic_54's art, drawn in a top and a bottom piece.
    float titleX = 8.0f;
    int32_t windowId = 0;
    int32_t windowType = 0;
    int32_t containerCount = 0;

    bool SameAs(const Layout& other) const
    {
        return windowId == other.windowId && windowType == other.windowType && containerCount == other.containerCount;
    }
};

Layout ComputeLayout(GlobalState* state)
{
    Layout layout;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        layout.windowId = state->inventory.activeWindowId;
        layout.windowType = state->inventory.activeWindowType;
        layout.containerCount = static_cast<int32_t>(state->inventory.containerSlots.size());
    }
    int32_t containerCount = layout.containerCount;
    std::vector<SlotDef>& slots = layout.slots;

    // The player's inventory and hotbar, placed by their first protocol index.
    auto addPlayerSlots = [&](int32_t firstMain, float mainY, int32_t firstHotbar, float hotbarY)
    {
        for (int row = 0; row < 3; row++)
        {
            for (int col = 0; col < 9; col++)
            {
                slots.push_back({ firstMain + row * 9 + col, 8.0f + static_cast<float>(col) * 18.0f, mainY + static_cast<float>(row) * 18.0f });
            }
        }
        for (int col = 0; col < 9; col++) slots.push_back({ firstHotbar + col, 8.0f + static_cast<float>(col) * 18.0f, hotbarY });
    };

    if (layout.windowId == 0)
    {
        for (int i = 0; i < 4; i++) slots.push_back({ 5 + i, 8.0f, 8.0f + static_cast<float>(i) * 18.0f });
        slots.push_back({ 45, 77.0f, 62.0f });
        for (int row = 0; row < 2; row++)
        {
            for (int col = 0; col < 2; col++)
            {
                slots.push_back({ 1 + row * 2 + col, 98.0f + static_cast<float>(col) * 18.0f, 18.0f + static_cast<float>(row) * 18.0f });
            }
        }
        slots.push_back({ 0, 154.0f, 28.0f });
        addPlayerSlots(9, 84.0f, 36, 142.0f);
    }
    else if (layout.windowType == InventoryManager::CRAFTING_MENU_TYPE && containerCount >= 10)
    {
        layout.background = std::string(ART) + "container/crafting_table.png";
        layout.titleX = 29.0f;
        slots.push_back({ 0, 124.0f, 35.0f });
        for (int row = 0; row < 3; row++)
        {
            for (int col = 0; col < 3; col++)
            {
                slots.push_back({ 1 + row * 3 + col, 30.0f + static_cast<float>(col) * 18.0f, 17.0f + static_cast<float>(row) * 18.0f });
            }
        }
        addPlayerSlots(containerCount, 84.0f, containerCount + 27, 142.0f);
    }
    else if (layout.windowType == InventoryManager::FURNACE_MENU_TYPE && containerCount >= 3)
    {
        layout.background = std::string(ART) + "container/furnace.png";
        slots.push_back({ 0, 56.0f, 17.0f });
        slots.push_back({ 1, 56.0f, 53.0f });
        slots.push_back({ 2, 116.0f, 35.0f });
        addPlayerSlots(containerCount, 84.0f, containerCount + 27, 142.0f);
    }
    else
    {
        // A chest-style grid, which is also the fallback for any menu without a layout of its own.
        layout.background = std::string(ART) + "container/generic_54.png";
        layout.genericRows = std::max(1, (containerCount + 8) / 9);
        layout.panelHeight = 114.0f + static_cast<float>(layout.genericRows) * 18.0f;
        for (int32_t i = 0; i < containerCount; i++)
        {
            slots.push_back({ i, 8.0f + static_cast<float>(i % 9) * 18.0f, 18.0f + static_cast<float>(i / 9) * 18.0f });
        }
        addPlayerSlots(containerCount, layout.panelHeight - 82.0f, containerCount + 27, layout.panelHeight - 24.0f);
    }
    return layout;
}

// ---- Tooltip text ----

// Vanilla's legacy formatting colors (the section sign plus 0-9, a-f).
bool LegacyColor(char code, Color& color)
{
    static const Color colors[16] = {
        { 0, 0, 0 }, { 0, 0, 170 }, { 0, 170, 0 }, { 0, 170, 170 }, { 170, 0, 0 }, { 170, 0, 170 }, { 255, 170, 0 }, { 170, 170, 170 },
        { 85, 85, 85 }, { 85, 85, 255 }, { 85, 255, 85 }, { 85, 255, 255 }, { 255, 85, 85 }, { 255, 85, 255 }, { 255, 255, 85 }, { 255, 255, 255 } };
    const char* digits = "0123456789abcdef";
    const char* at = std::strchr(digits, std::tolower(static_cast<unsigned char>(code)));
    if (code == '\0' || at == nullptr) return false;
    color = colors[at - digits];
    return true;
}

struct TooltipLine {
    std::string text; // May carry section-sign color codes (UTF-8 U+00A7).
    Color color{ 255, 255, 255 };
};

Color RarityColor(const std::string& name)
{
    if (name == "enchanted_golden_apple" || name == "dragon_egg" || name == "elytra" || name == "command_block")
        return Color{ 255, 85, 255 };
    if (name == "golden_apple" || name == "beacon" || name == "conduit" || name == "heart_of_the_sea"
        || name == "end_crystal" || name == "totem_of_undying" || name == "nether_star")
        return Color{ 85, 255, 255 };
    return Color{ 255, 255, 255 };
}

// Name, then food values, durability, and melee numbers where they apply.
std::vector<TooltipLine> TooltipLines(const ItemStack& item)
{
    std::vector<TooltipLine> lines;
    const ItemRegistry::ItemTypeInfo* info = ItemRegistry::Lookup(item.itemId);
    if (info == nullptr) return lines;

    lines.push_back({ info->displayName.empty() ? info->name : info->displayName, RarityColor(info->name) });

    char buffer[96];
    if (info->edible)
    {
        std::snprintf(buffer, sizeof(buffer), "Hunger: +%d", info->foodPoints);
        lines.push_back({ buffer, Color{ 255, 170, 0 } });
        std::snprintf(buffer, sizeof(buffer), "Saturation: +%.1f", static_cast<double>(info->saturation));
        lines.push_back({ buffer, Color{ 255, 220, 90 } });
    }

    int32_t maxDurability = item.maxDamage > 0 ? item.maxDamage : info->maxDurability;
    if (maxDurability > 0)
    {
        int32_t remaining = std::clamp(maxDurability - std::max(item.damage, 0), 0, maxDurability);
        float fraction = static_cast<float>(remaining) / static_cast<float>(maxDurability);
        std::snprintf(buffer, sizeof(buffer), "Durability: %d / %d", remaining, maxDurability);
        lines.push_back({ buffer, fraction > 0.5f ? Color{ 85, 255, 85 } : fraction > 0.2f ? Color{ 255, 255, 85 } : Color{ 255, 85, 85 } });
    }

    AttackStats attack = AttackStatsForItem(info->name);
    if (attack.valid)
    {
        lines.push_back({ " ", Color{ 255, 255, 255 } });
        lines.push_back({ "When in Main Hand:", Color{ 170, 170, 170 } });
        std::snprintf(buffer, sizeof(buffer), "%.4g Attack Damage (fully charged)", static_cast<double>(attack.damage));
        lines.push_back({ buffer, Color{ 85, 85, 255 } });
        std::snprintf(buffer, sizeof(buffer), "%.4g Attack Speed", static_cast<double>(attack.speed));
        lines.push_back({ buffer, Color{ 85, 85, 255 } });
        std::snprintf(buffer, sizeof(buffer), "Fully charged after %.2f s", static_cast<double>(1.0f / attack.speed));
        lines.push_back({ buffer, Color{ 170, 170, 170 } });
    }
    return lines;
}

// A line split on its color codes into spans.
std::vector<Pincel::TextSpan> SpansOf(const TooltipLine& line)
{
    std::vector<Pincel::TextSpan> spans;
    Color color = line.color;
    std::string segment;
    auto flush = [&]()
    {
        if (segment.empty()) return;
        Pincel::TextSpan span;
        span.text = segment;
        span.color = color;
        spans.push_back(std::move(span));
        segment.clear();
    };
    for (size_t i = 0; i < line.text.size(); i++)
    {
        bool sectionSign = static_cast<unsigned char>(line.text[i]) == 0xC2 && i + 2 < line.text.size()
            && static_cast<unsigned char>(line.text[i + 1]) == 0xA7;
        if (sectionSign)
        {
            char code = line.text[i + 2];
            flush();
            if (code == 'r' || code == 'R') color = line.color;
            else LegacyColor(code, color);
            i += 2;
            continue;
        }
        segment.push_back(line.text[i]);
    }
    flush();
    return spans;
}

// ---- The screen ----

struct InventoryView {
    Layout layout;
    std::vector<ItemSlotView> slots;
    Element title;
    Element highlightBack;
    Element highlightFront;
    Element cursor;          // Fixed, following the mouse, with the carried stack in it.
    ItemSlotView carried;
    Element tooltip;
    int32_t tooltipItem = -1;
    int32_t tooltipDamage = -1;
    DropKeyTracker dropKey;
};

Element AddArt(Element parent, const Sprite& sprite, int left, int top)
{
    Pincel::Insets at;
    at.left = left;
    at.top = top;
    auto image = sprite.AddTo(parent);
    if (!image) return Element();
    image->SetPosition(Position::Absolute).SetInsets(at).SetPointerEvents(PointerEvents::None);
    return *image;
}

void BuildInventory(ControlRenderer& ui, GlobalState* state, Screen& screen, InventoryView& view)
{
    int art = ui.Units(1.0f) + 1; // Screen pixels per vanilla texture pixel.
    int logical = 256;            // The container sheets' size in those, whatever the pack's resolution.
    auto pixels = [art](float texels) { return static_cast<int>(std::lround(texels * static_cast<float>(art))); };
    int fontPixels = (ui.Units(1.0f) + 1) * static_cast<int>(Theme::SMALL_FONT_SIZE);

    view.layout = ComputeLayout(state);
    const Layout& layout = view.layout;

    // The panel only holds things placed against it, and lets the mouse through to the backdrop.
    Element panel = ui.Column().AddStack(Orientation::Vertical);
    panel.SetSize(static_cast<uint16_t>(pixels(PANEL_WIDTH)), static_cast<uint16_t>(pixels(layout.panelHeight)))
        .SetPointerEvents(PointerEvents::None);

    Sprite background = Sprite::Cached(layout.background);
    if (layout.genericRows > 0)
    {
        int topHeight = 17 + layout.genericRows * 18;
        AddArt(panel, background.Region(logical, logical, 0, 0, 176, topHeight, art), 0, 0);
        AddArt(panel, background.Region(logical, logical, 0, 126, 176, 96, art), 0, pixels(static_cast<float>(topHeight)));
    }
    else
    {
        AddArt(panel, background.Region(logical, logical, 0, 0, 176, static_cast<int>(layout.panelHeight), art), 0, 0);
    }

    // The container's own name and "Inventory" over the player's part.
    view.title = Element();
    if (layout.windowId != 0)
    {
        Pincel::Insets titleAt;
        titleAt.left = pixels(layout.titleX);
        titleAt.top = pixels(6.0f);
        view.title = panel.AddText(screen.title);
        view.title.SetPosition(Position::Absolute).SetInsets(titleAt).SetForeground(LABEL_COLOR)
            .SetTextShadow(NO_SHADOW).SetPointerEvents(PointerEvents::None);

        Pincel::Insets inventoryAt;
        inventoryAt.left = pixels(8.0f);
        inventoryAt.top = pixels(layout.panelHeight - 94.0f);
        panel.AddText("Inventory").SetPosition(Position::Absolute).SetInsets(inventoryAt).SetForeground(LABEL_COLOR)
            .SetTextShadow(NO_SHADOW).SetPointerEvents(PointerEvents::None);
    }

    // The hover highlight goes behind the items and its front half over them.
    view.highlightBack = AddArt(panel, Sprite::Cached(std::string(ART) + "sprites/container/slot_highlight_back.png").Region(24, 24, 0, 0, 24, 24, art), 0, 0);
    view.highlightBack.SetVisible(false);

    view.slots.clear();
    for (const SlotDef& slot : layout.slots)
    {
        view.slots.push_back(ItemSlotView::Add(panel, pixels(slot.x), pixels(slot.y), pixels(ITEM_SIZE), fontPixels));
    }

    view.highlightFront = AddArt(panel, Sprite::Cached(std::string(ART) + "sprites/container/slot_highlight_front.png").Region(24, 24, 0, 0, 24, 24, art), 0, 0);
    view.highlightFront.SetVisible(false);

    // The stack on the cursor, and the item tooltip, over everything and out of the mouse's way.
    view.cursor = ui.Overlay().AddStack(Orientation::Vertical);
    view.cursor.SetPosition(Position::Fixed)
        .SetSize(static_cast<uint16_t>(pixels(ITEM_SIZE)), static_cast<uint16_t>(pixels(ITEM_SIZE)))
        .SetPointerEvents(PointerEvents::None);
    view.carried = ItemSlotView::Add(view.cursor, 0, 0, pixels(ITEM_SIZE), fontPixels);
    view.carried.Slot().SetPointerEvents(PointerEvents::None);

    view.tooltip = ui.Overlay().AddStack(Orientation::Vertical);
    view.tooltip.SetPosition(Position::Fixed)
        .SetPointerEvents(PointerEvents::None)
        .SetPadding(static_cast<uint16_t>(pixels(4.0f)))
        .SetBackground(Color{ 16, 0, 16, 240.0f / 255.0f })
        .SetBorder(static_cast<uint16_t>(std::max(1, pixels(1.0f))), Color{ 60, 8, 140 })
        .SetVisible(false);
    view.tooltipItem = -1;
    view.tooltipDamage = -1;
}

void UpdateInventory(ControlRenderer& ui, GlobalState* state, Screen& screen, InventoryView& view)
{
    // A different window (the player's, a chest, a furnace...) has different slots.
    if (!ComputeLayout(state).SameAs(view.layout))
    {
        ui.Rebuild();
        return;
    }
    int art = ui.Units(1.0f) + 1;
    auto pixels = [art](float texels) { return static_cast<int>(std::lround(texels * static_cast<float>(art))); };

    std::array<ItemStack, InventoryManager::ARMOR_SIZE> armorItems;
    std::array<ItemStack, 5> craftingItems; // 0 = result, 1-4 = 2x2 grid.
    ItemStack offhandItem;
    std::array<ItemStack, InventoryManager::MAIN_INVENTORY_SIZE> mainItems;
    std::array<ItemStack, HOTBAR_SLOTS> hotbarItems;
    std::vector<ItemStack> containerItems;
    int32_t activeWindowId;
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
        carriedItem = state->inventory.carriedItem;
    }
    int32_t containerCount = static_cast<int32_t>(containerItems.size());

    // The item at a protocol slot index in this layout.
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

    if (view.title.IsValid()) view.title.SetText(screen.title);
    const std::vector<SlotDef>& defs = view.layout.slots;
    for (size_t i = 0; i < defs.size(); i++) view.slots[i].Set(itemAt(defs[i].index));

    auto windowAndState = [&](int32_t& windowId, int32_t& stateId)
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        windowId = state->inventory.activeWindowId;
        stateId = windowId == 0 ? state->inventory.playerStateId : state->inventory.activeWindowStateId;
    };

    // A plain click: predicted here, then told to the server.
    auto slotClick = [&](int32_t protocolIndex, uint8_t button)
    {
        int32_t windowId, stateId;
        windowAndState(windowId, stateId);
        state->inventory.HandleSlotClick(protocolIndex, button);

        ItemStack slotItem;
        ItemStack cursorItem;
        {
            std::lock_guard<std::mutex> lock(state->inventory.mutex);
            ItemStack* slot = state->inventory.GetWindowSlot(protocolIndex);
            if (slot != nullptr) slotItem = *slot;
            cursorItem = state->inventory.carriedItem;
        }
        SendInventoryClick(state, windowId, stateId, protocolIndex, button, slotItem, cursorItem);
    };

    // Other click modes are left to the server: nothing is predicted, and the slots update when it answers.
    auto sendAction = [&](int32_t protocolIndex, uint8_t button, int32_t mode)
    {
        int32_t windowId, stateId;
        windowAndState(windowId, stateId);
        SendInventoryAction(state, windowId, stateId, protocolIndex, button, mode, carriedItem);
    };

    // Which slot is under the mouse, and which was clicked, with which button.
    int hovered = -1;
    bool slotClicked = false;
    bool shiftHeld = ImGui::GetIO().KeyShift;
    for (size_t i = 0; i < defs.size(); i++)
    {
        Element slot = view.slots[i].Slot();
        if (slot.IsHovered()) hovered = static_cast<int>(i);

        for (uint8_t button : { Pincel::MouseButton::Left, Pincel::MouseButton::Right })
        {
            if (slotClicked || !slot.WasClicked(button)) continue;
            slotClicked = true;
            uint8_t protocolButton = button == Pincel::MouseButton::Right ? 1 : 0;
            int32_t index = defs[i].index;
            if (shiftHeld && carriedItem.IsEmpty() && !itemAt(index).IsEmpty()) sendAction(index, protocolButton, 1); // Quick move.
            else slotClick(index, protocolButton);
        }
    }

    // A click on nothing drops what is on the cursor.
    Element backdrop = ui.Backdrop();
    for (uint8_t button : { Pincel::MouseButton::Left, Pincel::MouseButton::Right })
    {
        if (slotClicked || carriedItem.IsEmpty() || !backdrop.WasClicked(button)) continue;
        slotClicked = true;
        uint8_t protocolButton = button == Pincel::MouseButton::Right ? 1 : 0;

        int32_t windowId, stateId;
        windowAndState(windowId, stateId);
        state->inventory.HandleSlotClick(-999, protocolButton);
        ItemStack cursorItem;
        {
            std::lock_guard<std::mutex> lock(state->inventory.mutex);
            cursorItem = state->inventory.carriedItem;
        }
        SendInventoryClick(state, windowId, stateId, -999, protocolButton, ItemStack{}, cursorItem);
    }

    // The hotbar keys swap the hovered slot with that hotbar slot, and the drop key (tap one, hold or Ctrl the
    // stack) drops the hovered slot.
    DropKeyTracker::Result drop = view.dropKey.Update(state->input->IsActive("DropItem"), state->input->IsActive("DropItemHold"),
        state->input->IsActive("DropItemRelease"), ImGui::GetIO().KeyCtrl, ImGui::GetIO().DeltaTime);
    if (hovered >= 0 && carriedItem.IsEmpty())
    {
        int32_t index = defs[static_cast<size_t>(hovered)].index;
        for (int32_t hotbarSlot = 0; hotbarSlot < HOTBAR_SLOTS; hotbarSlot++)
        {
            if (state->input->WasActivated("Hotbar." + std::to_string(hotbarSlot)))
            {
                sendAction(index, static_cast<uint8_t>(hotbarSlot), 2);
                break;
            }
        }
        if (drop != DropKeyTracker::Result::NONE && !itemAt(index).IsEmpty())
        {
            sendAction(index, drop == DropKeyTracker::Result::STACK ? 1 : 0, 4);
        }
    }

    // The highlight around the hovered slot.
    for (Element highlight : { view.highlightBack, view.highlightFront })
    {
        highlight.SetVisible(hovered >= 0);
        if (hovered < 0) continue;
        Pincel::Insets at;
        at.left = pixels(defs[static_cast<size_t>(hovered)].x - 4.0f);
        at.top = pixels(defs[static_cast<size_t>(hovered)].y - 4.0f);
        highlight.SetInsets(at);
    }

    // The carried stack follows the cursor.
    const Pincel::MouseInput& mouse = ui.Mouse();
    view.carried.Set(carriedItem);
    view.cursor.SetVisible(!carriedItem.IsEmpty() && !std::isnan(mouse.x));
    if (!std::isnan(mouse.x))
    {
        Pincel::Insets at;
        at.left = static_cast<int>(mouse.x) - pixels(ITEM_SIZE) / 2;
        at.top = static_cast<int>(mouse.y) - pixels(ITEM_SIZE) / 2;
        view.cursor.SetInsets(at);
    }

    // The tooltip of the hovered stack (not while carrying one, as in vanilla).
    const ItemStack& hoveredItem = hovered >= 0 ? itemAt(defs[static_cast<size_t>(hovered)].index) : emptyItem;
    bool showTooltip = hovered >= 0 && carriedItem.IsEmpty() && !hoveredItem.IsEmpty() && !std::isnan(mouse.x);
    view.tooltip.SetVisible(showTooltip);
    if (!showTooltip) return;

    if (hoveredItem.itemId != view.tooltipItem || hoveredItem.damage != view.tooltipDamage)
    {
        view.tooltipItem = hoveredItem.itemId;
        view.tooltipDamage = hoveredItem.damage;
        view.tooltip.RemoveChildren();
        for (const TooltipLine& line : TooltipLines(hoveredItem))
        {
            std::vector<Pincel::TextSpan> spans = SpansOf(line);
            if (!spans.empty()) view.tooltip.AddRichText(spans);
        }
    }

    Element::Rect size = view.tooltip.GetRect();
    int x = static_cast<int>(mouse.x) + pixels(12.0f);
    int y = static_cast<int>(mouse.y) - pixels(12.0f);
    x = std::clamp(x, 0, std::max(0, ui.Width() - size.w));
    y = std::clamp(y, 0, std::max(0, ui.Height() - size.h));
    Pincel::Insets at;
    at.left = x;
    at.top = y;
    view.tooltip.SetInsets(at);
}

} // namespace

Screen* CreateInventoryScreen(GlobalState* state)
{
    Screen* screen = GUIController::CreateScreen("Inventory", /* closable */ true);
    screen->showTitle = false; // The panel carries its own.
    screen->fontSize = Theme::SMALL_FONT_SIZE;
    screen->fontInArtPixels = true;

    auto view = std::make_shared<InventoryView>();
    screen->build = [state, screen, view](ControlRenderer& ui) { BuildInventory(ui, state, *screen, *view); };
    screen->update = [state, screen, view](ControlRenderer& ui) { UpdateInventory(ui, state, *screen, *view); };
    return screen;
}

} // namespace Volcano::GUI
