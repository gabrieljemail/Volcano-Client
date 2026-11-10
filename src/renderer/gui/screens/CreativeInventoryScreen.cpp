#include "CreativeInventoryScreen.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <imgui.h>
#include "../ControlRenderer.hpp"
#include "../GUIController.hpp"
#include "../ItemSlotView.hpp"
#include "../PincelSprite.hpp"
#include "../Theme.hpp"
#include "DropKey.hpp"
#include "InputHandler.hpp"
#include "ItemTooltip.hpp"
#include "inventory/ItemRegistry.hpp"
#include "inventory/ItemStack.hpp"

// The network side, declared here rather than including NetworkClient.hpp (see GUIController.cpp for why).
namespace Volcano {
void SendSetCreativeSlot(GlobalState* state, int16_t slot, const ItemStack& item);
}

namespace Volcano::GUI {

namespace {

using Pincel::Color;
using Pincel::Element;
using Pincel::Orientation;
using Pincel::PointerEvents;
using Pincel::Position;

// All sizes below are in "art pixels", the same unit InventoryScreen uses:
// one vanilla texture pixel, which is (GUI scale + 1) screen pixels.
constexpr float SLOT_ART = 16.0f;
constexpr float PITCH_ART = 18.0f;
constexpr int GRID_COLS = 11;
constexpr int GRID_VIEW_ROWS = 11;
constexpr float GRID_GAP_ART = 8.0f;
constexpr float RAIL_WIDTH_ART = 28.0f; // Icon button (20) plus rail padding (8).
constexpr float SEARCH_HEIGHT_ART = 22.0f;
constexpr float PANEL_GAP_ART = 16.0f;
constexpr int SURVIVAL_SLOTS = 46; // Window-0 protocol indices 0..45.
constexpr const char* ART = "assets/minecraft/textures/gui/";

int Px(ControlRenderer& ui, float art)
{
    return static_cast<int>(std::lround(art * static_cast<float>(ui.Units(1.0f) + 1)));
}

// ---- The item catalog ----

// Creative tabs, in display order. Index 0 ("All") isn't a classification
// target; ClassifyTab returns 1..TabCount-1, and "Other" is the fallback.
struct TabDef {
    const char* name;
    const char* iconItem; // A representative item whose sprite labels the tab.
};

const std::vector<TabDef>& Tabs()
{
    static const std::vector<TabDef> tabs = {
        { "All Items", "nether_star" },
        { "Building Blocks", "bricks" },
        { "Natural", "oak_log" },
        { "Functional", "crafting_table" },
        { "Redstone", "redstone" },
        { "Equipment", "diamond_sword" },
        { "Food & Drinks", "cooked_beef" },
        { "Spawn Eggs", "zombie_spawn_egg" },
        { "Other", "stick" },
    };
    return tabs;
}

bool NameHas(const std::string& name, std::initializer_list<const char*> keywords)
{
    for (const char* keyword : keywords)
        if (name.find(keyword) != std::string::npos) return true;
    return false;
}

bool EndsWith(const std::string& name, const char* suffix)
{
    size_t n = std::strlen(suffix);
    return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
}

// First matching tab wins, so order is significant: spawn eggs and food are
// attributes, then gear, redstone, functional blocks, building blocks, and
// natural blocks; whatever is left lands in "Other". These are name heuristics
// - good enough to browse, and search covers the misses.
int ClassifyTab(const std::string& name, const ItemRegistry::ItemTypeInfo& info)
{
    if (EndsWith(name, "_spawn_egg")) return 7;
    if (info.edible) return 6;
    if (NameHas(name, { "sword", "pickaxe", "axe", "shovel", "hoe", "helmet", "chestplate", "leggings",
            "boots", "elytra", "shield", "bow", "crossbow", "trident", "mace", "fishing_rod", "goat_horn",
            "brush", "carrot_on_a_stick", "warped_fungus_on_a_stick", "wind_charge", "totem", "ender_pearl",
            "ender_eye", "fire_charge", "snowball", "potion", "arrow", "armor" })) return 5;
    if (NameHas(name, { "redstone", "repeater", "comparator", "piston", "observer", "hopper", "dropper",
            "dispenser", "daylight_detector", "target", "note_block", "tripwire", "sculk_sensor",
            "calibrated_sculk", "crafter", "lever", "button", "pressure_plate", "tnt", "rail",
            "slime_block", "honey_block", "heavy_core", "trial_spawner", "vault" })) return 4;
    if (NameHas(name, { "crafting", "furnace", "chest", "barrel", "smoker", "stonecutter", "grindstone",
            "smithing", "fletching", "cartography", "loom", "composter", "lectern", "anvil", "enchant",
            "brewing", "cauldron", "_bed", "door", "trapdoor", "fence", "wall", "sign", "banner", "candle",
            "lantern", "torch", "campfire", "chain", "lodestone", "anchor", "conduit", "beacon", "bell",
            "bookshelf", "jukebox", "frame", "name_tag", "clock", "compass", "spyglass", "bundle", "shulker",
            "lead", "pot", "skull", "head" })) return 3;
    if (NameHas(name, { "stone", "brick", "planks", "wool", "concrete", "terracotta", "glass", "sandstone",
            "quartz", "obsidian", "magma", "glowstone", "sea_lantern", "copper", "ingot", "nugget",
            "diamond_block", "emerald_block", "lapis_block", "gold_block", "iron_block", "coal_block",
            "netherite_block", "raw_", "deepslate", "cobble", "tile", "resin" })) return 1;
    if (NameHas(name, { "log", "leaves", "sapling", "flower", "mushroom", "_ore", "dirt", "sand", "gravel",
            "clay", "ice", "snow", "coral", "kelp", "seagrass", "vine", "moss", "nylium", "basalt",
            "blackstone", "netherrack", "soul_", "amethyst", "dripstone", "rooted", "honey", "slime",
            "prismarine", "granite", "diorite", "andesite", "tuff", "calcite", "bamboo", "sugar_cane",
            "cactus", "lily", "grass", "fern", "berry", "lichen", "sculk", "wart", "fungus", "crimson",
            "warped", "shroomlight", "dripleaf", "spore", "root", "mycelium", "podzol", "mud" })) return 2;
    return 8;
}

struct CatalogEntry {
    int32_t id;
    std::string name;
    std::string display;
    int tab;
};

struct Catalog {
    std::vector<CatalogEntry> items; // Sorted by display name.
    std::unordered_map<std::string, int32_t> idByName;
};

const Catalog& GetCatalog()
{
    static const Catalog catalog = []
    {
        Catalog cat;
        ItemRegistry::ForEach([&cat](int32_t id, const ItemRegistry::ItemTypeInfo& info)
        {
            cat.idByName.emplace(info.name, id);
            cat.items.push_back({ id, info.name, info.displayName.empty() ? info.name : info.displayName,
                ClassifyTab(info.name, info) });
        });
        std::sort(cat.items.begin(), cat.items.end(), [](const CatalogEntry& a, const CatalogEntry& b)
        {
            return a.display < b.display;
        });
        return cat;
    }();
    return catalog;
}

bool NameMatches(const CatalogEntry& entry, const std::string& needle)
{
    auto contains = [&needle](const std::string& text)
    {
        std::string haystack = text;
        std::string lower = needle;
        std::transform(haystack.begin(), haystack.end(), haystack.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return haystack.find(lower) != std::string::npos;
    };
    return contains(entry.name) || contains(entry.display);
}

// ---- The screen ----

struct GridCell {
    ItemSlotView view;
    int32_t itemId = 0;
    int row = 0;
};

struct CreativeView {
    // Search and tabs.
    Element searchBody;
    Element searchInput;
    Element rail;
    std::vector<Element> tabButtons;
    Element destroyButton;
    int currentTab = 0;
    std::string searchText;
    std::string gridKey;
    bool gridDirty = true;

    // The picker grid.
    Element gridViewport;
    Element gridRows;
    std::vector<GridCell> cells;
    std::vector<int32_t> visibleItemIds; // Parallel to cells, for clicks/tooltips.

    // Category title, cursor-carried stack, tooltip.
    Element categoryTitle;
    Element cursor;
    ItemSlotView carried;
    Element tooltip;
    int32_t tooltipItem = -1;
    int32_t tooltipDamage = -1;

    // The survival-inventory panel (window-0 layout).
    std::vector<ItemSlotView> survivalSlots;
    std::vector<int32_t> survivalIndices;

    DropKeyTracker dropKey;
};

// Window-0 slot coordinates, in vanilla texture pixels - the same layout
// InventoryScreen uses for the player's own inventory.
const std::vector<std::pair<int32_t, std::pair<float, float>>>& SurvivalSlotDefs()
{
    static const std::vector<std::pair<int32_t, std::pair<float, float>>> defs = []
    {
        std::vector<std::pair<int32_t, std::pair<float, float>>> d;
        for (int i = 0; i < 4; i++) d.push_back({ 5 + i, { 8.0f, 8.0f + static_cast<float>(i) * 18.0f } });
        d.push_back({ 45, { 77.0f, 62.0f } });
        for (int row = 0; row < 2; row++)
            for (int col = 0; col < 2; col++)
                d.push_back({ 1 + row * 2 + col, { 98.0f + static_cast<float>(col) * 18.0f, 18.0f + static_cast<float>(row) * 18.0f } });
        d.push_back({ 0, { 154.0f, 28.0f } });
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 9; col++)
                d.push_back({ 9 + row * 9 + col, { 8.0f + static_cast<float>(col) * 18.0f, 84.0f + static_cast<float>(row) * 18.0f } });
        for (int col = 0; col < 9; col++)
            d.push_back({ 36 + col, { 8.0f + static_cast<float>(col) * 18.0f, 142.0f } });
        return d;
    }();
    return defs;
}

// The item at a window-0 protocol index, from a snapshot.
const ItemStack& ItemAt(const std::array<ItemStack, SURVIVAL_SLOTS>& snapshot, int32_t index)
{
    static const ItemStack empty{};
    if (index < 0 || index >= SURVIVAL_SLOTS) return empty;
    return snapshot[static_cast<size_t>(index)];
}

void SnapshotWindow0(GlobalState* state, std::array<ItemStack, SURVIVAL_SLOTS>& out)
{
    std::lock_guard<std::mutex> lock(state->inventory.mutex);
    for (int32_t i = 0; i < SURVIVAL_SLOTS; i++)
    {
        ItemStack* slot = state->inventory.GetSlot(i);
        out[static_cast<size_t>(i)] = slot != nullptr ? *slot : ItemStack{};
    }
}

bool SameStack(const ItemStack& a, const ItemStack& b)
{
    return a.itemId == b.itemId && a.count == b.count && a.damage == b.damage
        && a.maxDamage == b.maxDamage && a.glint == b.glint;
}

// ---- Building ----

Sprite FitIcon(const Sprite& source, int size)
{
    if (source.width == source.height && size % source.width == 0) return source.ScaledNearest(size / source.width);
    return source.Resized(size, size);
}

void AddTabIcon(Element button, const TabDef& tab, int iconSize)
{
    auto it = GetCatalog().idByName.find(tab.iconItem);
    if (it == GetCatalog().idByName.end()) return;
    const Sprite* sprite = GUIController::GetItemSprite(ItemStack{ it->second, 1 });
    if (sprite == nullptr || sprite->IsEmpty()) return;
    Sprite fitted = FitIcon(*sprite, iconSize);
    if (auto image = fitted.AddTo(button))
    {
        image->SetPointerEvents(PointerEvents::None);
    }
}

void StyleTab(Element button, bool selected)
{
    button.SetBackground(selected ? Theme::PANEL_LIGHT : Theme::PANEL_MID)
        .SetBorder(1, selected ? Theme::ORANGE : Theme::PANEL_LIGHT);
}

void BuildSearch(ControlRenderer& ui, Element parent, CreativeView& view, int widthPx, int heightPx)
{
    int fontPixels = (ui.Units(1.0f) + 1) * static_cast<int>(Theme::SMALL_FONT_SIZE);
    int padX = Px(ui, 8.0f);
    int padY = std::max(0, (heightPx - fontPixels) / 2); // Centers the single line of text.
    view.searchBody = parent.AddStack(Orientation::Vertical);
    view.searchBody.SetSize(static_cast<uint16_t>(widthPx), static_cast<uint16_t>(heightPx))
        .SetBackground(Theme::PANEL_MID)
        .SetBorder(1, Theme::PANEL_LIGHT)
        .SetCornerRadius(6)
        .SetPadding(padY, padX, padY, padX);
    view.searchInput = view.searchBody.AddInput(Pincel::InputKind::Text);
    view.searchInput.SetSize(static_cast<uint16_t>(std::max(1, widthPx - 2 * (padX + 1))), 0)
        .SetPlaceholder("Search items by name or ID...");
}

void BuildCreative(ControlRenderer& ui, GlobalState* state, Screen& screen, CreativeView& view)
{
    int fontPixels = (ui.Units(1.0f) + 1) * static_cast<int>(Theme::SMALL_FONT_SIZE);
    const int slotPx = Px(ui, SLOT_ART);
    const int pitchPx = Px(ui, PITCH_ART);
    // Cells sit at x = 1 + col * pitch, `slotPx` wide: the row is as wide as
    // its last cell's right edge.
    const int gridW = 1 + (GRID_COLS - 1) * pitchPx + slotPx;
    const int gridH = pitchPx * GRID_VIEW_ROWS;
    const int railW = Px(ui, RAIL_WIDTH_ART);
    const int railH = Px(ui, 10.0f + GRID_GAP_ART + static_cast<float>(GRID_VIEW_ROWS) * PITCH_ART);
    const int leftW = railW + Px(ui, GRID_GAP_ART) + gridW;
    const int gapPx = Px(ui, GRID_GAP_ART);
    const int panelGapPx = Px(ui, PANEL_GAP_ART);

    // The design's root: [search + (rail | grid)] [survival inventory]. The
    // horizontal stack centers its children vertically, so the shorter
    // survival panel sits in the middle of the tall picker column.
    Element root = ui.Column().AddStack(Orientation::Horizontal);
    root.SetGap(static_cast<uint16_t>(panelGapPx))
        .SetAlign(Pincel::Align::Start, Pincel::Align::Center);

    Element left = root.AddStack(Orientation::Vertical);
    left.SetGap(static_cast<uint16_t>(gapPx));

    BuildSearch(ui, left, view, leftW, Px(ui, SEARCH_HEIGHT_ART));

    Element mid = left.AddStack(Orientation::Horizontal);
    mid.SetGap(static_cast<uint16_t>(gapPx));

    // The category rail: icon buttons down the side, the destroy slot pinned
    // to its bottom like vanilla's. Sized to the buttons plus padding, not a
    // fixed width - a wider rail just looks empty.
    view.rail = mid.AddStack(Orientation::Vertical);
    Element& rail = view.rail;
    rail.SetSize(0, static_cast<uint16_t>(railH)) // Width auto: buttons plus padding.
        .SetPadding(Px(ui, 4.0f))
        .SetBackground(Theme::PANEL_DARK)
        .SetCornerRadius(4)
        .SetGap(Px(ui, 2.0f));

    const std::vector<TabDef>& tabs = Tabs();
    view.tabButtons.clear();
    int tabIconPx = slotPx - Px(ui, 2.0f);
    for (size_t i = 0; i < tabs.size(); i++)
    {
        Element button = rail.AddStack(Orientation::Vertical);
        button.SetSize(static_cast<uint16_t>(slotPx + Px(ui, 4.0f)), static_cast<uint16_t>(slotPx + Px(ui, 4.0f)))
            .SetAlign(Pincel::Align::Center, Pincel::Align::Center)
            .SetCornerRadius(4)
            .SetBackground(Theme::PANEL_MID);
        AddTabIcon(button, tabs[i], tabIconPx);
        view.tabButtons.push_back(button);
    }

    view.destroyButton = rail.AddStack(Orientation::Vertical);
    view.destroyButton.SetPosition(Position::Absolute)
        .SetSize(static_cast<uint16_t>(slotPx + Px(ui, 4.0f)), static_cast<uint16_t>(slotPx + Px(ui, 4.0f)))
        .SetAlign(Pincel::Align::Center, Pincel::Align::Center)
        .SetCornerRadius(4)
        .SetBackground(Theme::PANEL_MID);
    Pincel::Insets destroyAt;
    destroyAt.left = Px(ui, 4.0f); // Under the tab buttons, which start after the rail's padding.
    destroyAt.bottom = 4;
    view.destroyButton.SetInsets(destroyAt);
    {
        auto it = GetCatalog().idByName.find("barrier");
        if (it != GetCatalog().idByName.end())
        {
            if (const Sprite* sprite = GUIController::GetItemSprite(ItemStack{ it->second, 1 }))
            {
                Sprite fitted = FitIcon(*sprite, tabIconPx);
                if (auto image = fitted.AddTo(view.destroyButton)) image->SetPointerEvents(PointerEvents::None);
            }
        }
    }

    // Category name over the scrolling item grid.
    Element gridArea = mid.AddStack(Orientation::Vertical);
    gridArea.SetGap(static_cast<uint16_t>(Px(ui, 4.0f)));
    view.categoryTitle = gridArea.AddText(Tabs()[view.currentTab].name);
    view.categoryTitle.SetFontSize(static_cast<uint16_t>(fontPixels))
        .SetForeground(Theme::LABEL_TEXT);

    view.gridViewport = gridArea.AddStack(Orientation::Vertical);
    view.gridViewport.SetSize(static_cast<uint16_t>(gridW), static_cast<uint16_t>(gridH))
        .SetBackground(Theme::PANEL_DARK)
        .SetBorder(1, Theme::PANEL_LIGHT)
        .SetCornerRadius(4)
        .SetOverflow(Pincel::Overflow::Scroll);
    view.gridRows = view.gridViewport.AddStack(Orientation::Vertical);

    // The player's own inventory, in vanilla's window-0 arrangement on the
    // right of the picker.
    Element panel = root.AddStack(Orientation::Vertical);
    panel.SetSize(Px(ui, 176.0f), Px(ui, 166.0f))
        .SetPointerEvents(PointerEvents::None);

    Sprite background = Sprite::Cached(std::string(ART) + "container/inventory.png");
    if (auto image = background.Region(256, 256, 0, 0, 176, 166, Px(ui, 1.0f) + 1).AddTo(panel))
    {
        Pincel::Insets at;
        at.left = 0;
        at.top = 0;
        image->SetPosition(Position::Absolute).SetInsets(at).SetPointerEvents(PointerEvents::None);
    }

    view.survivalSlots.clear();
    view.survivalIndices.clear();
    for (const auto& [index, pos] : SurvivalSlotDefs())
    {
        view.survivalSlots.push_back(ItemSlotView::Add(panel, Px(ui, pos.first), Px(ui, pos.second), slotPx, fontPixels));
        // Slot frames come from the panel art; give the slot a hover highlight
        // so clicks read the same as the picker's.
        view.survivalSlots.back().Slot().SetBackground(Color{ 255, 255, 255, 0.0f })
            .SetHoverBackground(Color{ 255, 255, 255, 0.18f });
        view.survivalIndices.push_back(index);
    }

    // The stack on the cursor, and the item tooltip, over everything.
    view.cursor = ui.Overlay().AddStack(Orientation::Vertical);
    view.cursor.SetPosition(Position::Fixed)
        .SetSize(static_cast<uint16_t>(slotPx), static_cast<uint16_t>(slotPx))
        .SetPointerEvents(PointerEvents::None);
    view.carried = ItemSlotView::Add(view.cursor, 0, 0, slotPx, fontPixels);
    view.carried.Slot().SetPointerEvents(PointerEvents::None);

    view.tooltip = ui.Overlay().AddStack(Orientation::Vertical);
    view.tooltip.SetPosition(Position::Fixed)
        .SetPointerEvents(PointerEvents::None)
        .SetPadding(Px(ui, 4.0f))
        .SetBackground(Color{ 16, 0, 16, 240.0f / 255.0f })
        .SetBorder(std::max(1, Px(ui, 1.0f)), Color{ 60, 8, 140 })
        .SetVisible(false);
    view.tooltipItem = -1;
    view.tooltipDamage = -1;
}

// Rebuilds the picker grid for the current tab/search. Only runs when the
// filter actually changed - the element churn is too big for per-frame.
void RebuildGrid(ControlRenderer& ui, CreativeView& view)
{
    const Catalog& catalog = GetCatalog();
    const std::string& needle = view.searchText;
    bool searching = !needle.empty();

    view.cells.clear();
    view.visibleItemIds.clear();
    view.gridRows.RemoveChildren();

    const int slotPx = Px(ui, SLOT_ART);
    const int pitchPx = Px(ui, PITCH_ART);
    const int rowW = 1 + (GRID_COLS - 1) * pitchPx + slotPx;
    const int rowH = pitchPx;
    const int fontPixels = (ui.Units(1.0f) + 1) * static_cast<int>(Theme::SMALL_FONT_SIZE);

    int row = 0;
    int col = 0;
    Element rowEl;
    auto nextRow = [&]()
    {
        rowEl = view.gridRows.AddStack(Orientation::Vertical);
        rowEl.SetSize(static_cast<uint16_t>(rowW), static_cast<uint16_t>(rowH));
        col = 0;
    };
    nextRow();

    auto addCell = [&](int32_t itemId)
    {
        if (col >= GRID_COLS) { row++; nextRow(); }
        GridCell cell;
        cell.view = ItemSlotView::Add(rowEl, 1 + col * pitchPx, 1, slotPx, fontPixels);
        cell.view.Slot().SetBackground(Theme::PANEL_MID).SetCornerRadius(2)
            .SetHoverBackground(Theme::PANEL_LIGHT);
        cell.itemId = itemId;
        cell.row = row;
        view.cells.push_back(cell);
        view.visibleItemIds.push_back(itemId);
        col++;
    };

    if (searching)
    {
        for (const CatalogEntry& entry : catalog.items)
            if (NameMatches(entry, needle)) addCell(entry.id);
    }
    else if (view.currentTab == 0)
    {
        for (const CatalogEntry& entry : catalog.items) addCell(entry.id);
    }
    else
    {
        for (const CatalogEntry& entry : catalog.items)
            if (entry.tab == view.currentTab) addCell(entry.id);
    }

    view.gridViewport.SetScroll(0, 0);
    view.gridDirty = false;
}

// ---- Reacting ----

// Picker clicks only move the client-side cursor stack; nothing is sent.
// The picker is an infinite source, so a different item replaces the carried
// stack and the same item tops it up to a full stack.
void PickerClick(GlobalState* state, int32_t itemId, uint8_t button)
{
    const ItemRegistry::ItemTypeInfo* info = ItemRegistry::Lookup(itemId);
    int32_t maxStack = info != nullptr ? info->maxStackSize : 64;
    int32_t count = button == Pincel::MouseButton::Right ? (maxStack + 1) / 2 : maxStack;

    std::lock_guard<std::mutex> lock(state->inventory.mutex);
    ItemStack& carried = state->inventory.carriedItem;
    if (carried.IsEmpty() || carried.itemId != itemId)
    {
        carried = ItemStack{ itemId, static_cast<uint8_t>(std::clamp(count, 1, 255)) };
    }
    else if (button != Pincel::MouseButton::Right)
    {
        carried.count = static_cast<uint8_t>(std::clamp(maxStack, 1, 255));
    }
}

// Sends SetCreativeSlot for every window-0 slot that differs between two
// snapshots - the diff half of SurvivalClick/QuickMove.
void ReportChanges(GlobalState* state, const std::array<ItemStack, SURVIVAL_SLOTS>& before,
    const std::array<ItemStack, SURVIVAL_SLOTS>& after)
{
    for (int32_t i = 0; i < SURVIVAL_SLOTS; i++)
        if (!SameStack(before[static_cast<size_t>(i)], after[static_cast<size_t>(i)]))
            SendSetCreativeSlot(state, static_cast<int16_t>(i), after[static_cast<size_t>(i)]);
}

// A player-inventory click: predicted with the same logic the survival screen
// uses, then every slot that actually changed is reported with SetCreativeSlot -
// vanilla's CreativeInventoryListener does exactly this per slotChanged.
void SurvivalClick(GlobalState* state, int32_t index, uint8_t button)
{
    std::array<ItemStack, SURVIVAL_SLOTS> before;
    SnapshotWindow0(state, before);
    state->inventory.HandleSlotClick(index, button);
    std::array<ItemStack, SURVIVAL_SLOTS> after;
    SnapshotWindow0(state, after);
    ReportChanges(state, before, after);
}

// Shift-click quick move between the main inventory and the hotbar: the whole
// stack goes to the first slot with room (same item first, then empty), or
// stays put when the other section is full.
void QuickMove(GlobalState* state, int32_t index)
{
    int32_t first, count;
    if (index >= 9 && index <= 35) { first = 36; count = 9; }
    else if (index >= 36 && index <= 44) { first = 9; count = 27; }
    else return;

    std::array<ItemStack, SURVIVAL_SLOTS> before;
    SnapshotWindow0(state, before);
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        ItemStack* src = state->inventory.GetSlot(index);
        if (src == nullptr || src->IsEmpty()) return;
        ItemStack moving = *src;
        *src = ItemStack{};

        for (int pass = 0; pass < 2 && !moving.IsEmpty(); pass++)
        {
            for (int i = 0; i < count && !moving.IsEmpty(); i++)
            {
                ItemStack* dst = state->inventory.GetSlot(first + i);
                if (dst == nullptr) continue;
                if (pass == 0)
                {
                    if (dst->IsEmpty() || dst->itemId != moving.itemId || dst->count >= 64) continue;
                }
                else if (!dst->IsEmpty()) continue;

                int32_t moved = std::min<int32_t>(64 - dst->count, moving.count);
                dst->itemId = moving.itemId;
                dst->count = static_cast<uint8_t>(dst->count + moved);
                dst->damage = moving.damage;
                dst->maxDamage = moving.maxDamage;
                dst->glint = moving.glint;
                moving.count = static_cast<uint8_t>(moving.count - moved);
                if (moving.count == 0) moving = ItemStack{};
            }
        }
        if (!moving.IsEmpty()) *src = moving; // Nowhere to go: put it back.
    }
    std::array<ItemStack, SURVIVAL_SLOTS> after;
    SnapshotWindow0(state, after);
    ReportChanges(state, before, after);
}

// Clicking the backdrop drops the carried stack into the world (slot -1).
void BackdropClick(GlobalState* state, uint8_t button)
{
    ItemStack before;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        before = state->inventory.carriedItem;
    }
    if (before.IsEmpty()) return;

    state->inventory.HandleSlotClick(-999, button);

    ItemStack after;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        after = state->inventory.carriedItem;
    }
    if (before.itemId == after.itemId && before.count > after.count)
    {
        ItemStack dropped = before;
        dropped.count = static_cast<uint8_t>(before.count - after.count);
        SendSetCreativeSlot(state, -1, dropped);
    }
    else if (after.IsEmpty())
    {
        SendSetCreativeSlot(state, -1, before);
    }
}

// Shift+click on the destroy slot empties the whole player inventory; a plain
// click just clears the cursor stack (the cursor is client-side only).
void DestroyClick(GlobalState* state, bool shiftHeld)
{
    if (!shiftHeld)
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        state->inventory.carriedItem = ItemStack{};
        return;
    }

    std::vector<std::pair<int16_t, ItemStack>> cleared;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        for (int32_t i = 0; i < SURVIVAL_SLOTS; i++)
        {
            ItemStack* slot = state->inventory.GetSlot(i);
            if (slot != nullptr && !slot->IsEmpty())
            {
                cleared.push_back({ static_cast<int16_t>(i), ItemStack{} });
                *slot = ItemStack{};
            }
        }
        state->inventory.carriedItem = ItemStack{};
    }
    for (const auto& [slot, empty] : cleared) SendSetCreativeSlot(state, slot, empty);
}

// Number keys: on a survival slot they swap it with that hotbar slot; on a
// picker item they overwrite the hotbar slot with a full stack (vanilla's
// creative SWAP behavior).
void HotbarKey(GlobalState* state, int32_t hoveredSurvival, int32_t hoveredPicker, int hotbarSlot)
{
    if (hoveredSurvival >= 0)
    {
        int32_t hotbarIndex = 36 + hotbarSlot;
        ItemStack a, b;
        {
            std::lock_guard<std::mutex> lock(state->inventory.mutex);
            ItemStack* pa = state->inventory.GetSlot(hoveredSurvival);
            ItemStack* pb = state->inventory.GetSlot(hotbarIndex);
            if (pa == nullptr || pb == nullptr) return;
            a = *pa;
            b = *pb;
            *pa = b;
            *pb = a;
        }
        SendSetCreativeSlot(state, static_cast<int16_t>(hoveredSurvival), b);
        SendSetCreativeSlot(state, static_cast<int16_t>(hotbarIndex), a);
    }
    else if (hoveredPicker >= 0)
    {
        const ItemRegistry::ItemTypeInfo* info = ItemRegistry::Lookup(hoveredPicker);
        int32_t maxStack = info != nullptr ? info->maxStackSize : 64;
        ItemStack stack{ hoveredPicker, static_cast<uint8_t>(std::clamp(maxStack, 1, 255)) };
        int32_t hotbarIndex = 36 + hotbarSlot;
        {
            std::lock_guard<std::mutex> lock(state->inventory.mutex);
            ItemStack* pb = state->inventory.GetSlot(hotbarIndex);
            if (pb != nullptr) *pb = stack;
        }
        SendSetCreativeSlot(state, static_cast<int16_t>(hotbarIndex), stack);
    }
}

// Q-style drop on a survival slot: the dropped part goes to the world (slot
// -1), the remainder stays and is reported.
void DropSurvivalSlot(GlobalState* state, int32_t index, int32_t amount)
{
    ItemStack stack;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        ItemStack* slot = state->inventory.GetSlot(index);
        if (slot == nullptr) return;
        stack = *slot;
    }
    if (stack.IsEmpty()) return;

    int32_t n = std::min<int32_t>(amount, stack.count);
    ItemStack dropped = stack;
    dropped.count = static_cast<uint8_t>(n);
    ItemStack remaining = stack;
    remaining.count = static_cast<uint8_t>(stack.count - n);
    if (remaining.count == 0) remaining = ItemStack{};

    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        state->inventory.SetSlot(index, remaining);
    }
    SendSetCreativeSlot(state, -1, dropped);
    SendSetCreativeSlot(state, static_cast<int16_t>(index), remaining);
}

// Shift+MMB deletes the hovered survival slot outright (Bedrock's Y-delete);
// plain MMB clones the slot's item onto the cursor at max stack.
void MiddleClickSurvival(GlobalState* state, int32_t index, bool shiftHeld)
{
    ItemStack stack;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        ItemStack* slot = state->inventory.GetSlot(index);
        if (slot == nullptr) return;
        stack = *slot;
    }
    if (stack.IsEmpty()) return;

    if (shiftHeld)
    {
        {
            std::lock_guard<std::mutex> lock(state->inventory.mutex);
            state->inventory.SetSlot(index, ItemStack{});
        }
        SendSetCreativeSlot(state, static_cast<int16_t>(index), ItemStack{});
        return;
    }

    const ItemRegistry::ItemTypeInfo* info = ItemRegistry::Lookup(stack.itemId);
    int32_t maxStack = info != nullptr ? info->maxStackSize : 64;
    ItemStack clone = stack;
    clone.count = static_cast<uint8_t>(std::clamp(maxStack, 1, 255));
    std::lock_guard<std::mutex> lock(state->inventory.mutex);
    state->inventory.carriedItem = clone;
}

void UpdateCreative(ControlRenderer& ui, GlobalState* state, Screen& screen, CreativeView& view)
{
    int fontPixels = (ui.Units(1.0f) + 1) * static_cast<int>(Theme::SMALL_FONT_SIZE);
    const int slotPx = Px(ui, SLOT_ART);
    const int pitchPx = Px(ui, PITCH_ART);

    view.searchBody.SetBorder(1, view.searchInput.IsFocused() ? Theme::ORANGE : Theme::PANEL_LIGHT);

    // Search text and tab clicks decide when the grid rebuilds.
    std::string searchText = view.searchInput.GetText();
    if (view.searchInput.InputChanged() || searchText != view.searchText)
    {
        view.searchText = searchText;
        view.gridDirty = true;
    }
    const std::vector<TabDef>& tabs = Tabs();
    // Clicks on any widget must not reach the backdrop's drop-the-cursor path:
    // WasClicked bubbles up from children, so the backdrop alone can't tell a
    // widget click from an empty-space click.
    bool widgetClick = view.destroyButton.ClickedButtons() != 0
        || view.searchBody.ClickedButtons() != 0
        || view.rail.ClickedButtons() != 0
        || view.gridViewport.ClickedButtons() != 0;
    for (size_t i = 0; i < view.tabButtons.size(); i++)
    {
        StyleTab(view.tabButtons[i], static_cast<int>(i) == view.currentTab);
        if (view.tabButtons[i].ClickedButtons() != 0) widgetClick = true;
        if (view.tabButtons[i].WasClicked(Pincel::MouseButton::Left))
        {
            view.currentTab = static_cast<int>(i);
            view.gridDirty = true;
        }
    }

    std::string key = std::to_string(view.currentTab) + "|" + (searchText.empty() ? "\x01" : searchText);
    if (key != view.gridKey || view.gridDirty)
    {
        view.gridKey = key;
        RebuildGrid(ui, view);
        view.categoryTitle.SetText(tabs[view.currentTab].name);
    }

    // Inventory snapshot for the survival panel and the cursor stack.
    std::array<ItemStack, SURVIVAL_SLOTS> snapshot;
    SnapshotWindow0(state, snapshot);
    ItemStack carriedItem;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        carriedItem = state->inventory.carriedItem;
    }

    // Picker cells: only the rows near the scroll offset get their icons
    // resolved - the rest stay blank until scrolled to, so opening the screen
    // doesn't render every item's 3D icon at once.
    Pincel::ScrollState scroll = view.gridViewport.GetScroll();
    int viewH = Px(ui, static_cast<float>(GRID_VIEW_ROWS) * PITCH_ART);
    int firstRow = std::max(0, scroll.y / pitchPx - 1);
    int lastRow = std::min(static_cast<int>(view.cells.size() > 0 ? view.cells.back().row : 0),
        (scroll.y + viewH) / pitchPx + 1);

    int hoveredPicker = -1;
    bool pickerClicked = false;
    int hoveredSurvival = -1;
    bool survivalClicked = false;
    bool shiftHeld = ImGui::GetIO().KeyShift;

    for (size_t i = 0; i < view.cells.size(); i++)
    {
        GridCell& cell = view.cells[i];
        Element slot = cell.view.Slot();
        if (cell.row >= firstRow && cell.row <= lastRow)
            cell.view.Set(ItemStack{ cell.itemId, 1 });
        if (slot.IsHovered()) hoveredPicker = static_cast<int>(i);

        for (uint8_t button : { Pincel::MouseButton::Left, Pincel::MouseButton::Right, Pincel::MouseButton::Middle })
        {
            if (pickerClicked || !slot.WasClicked(button)) continue;
            pickerClicked = true;
            PickerClick(state, cell.itemId, button);
        }
    }

    for (size_t i = 0; i < view.survivalSlots.size(); i++)
    {
        Element slot = view.survivalSlots[i].Slot();
        view.survivalSlots[i].Set(ItemAt(snapshot, view.survivalIndices[i]));
        if (slot.IsHovered()) hoveredSurvival = static_cast<int>(i);

        if (slot.WasClicked(Pincel::MouseButton::Left) || slot.WasClicked(Pincel::MouseButton::Right))
        {
            if (survivalClicked) continue;
            survivalClicked = true;
            uint8_t button = slot.WasClicked(Pincel::MouseButton::Right) ? Pincel::MouseButton::Right
                : Pincel::MouseButton::Left;
            if (shiftHeld && carriedItem.IsEmpty())
            {
                // Shift-click quick-moves between the inventory sections.
                int32_t index = view.survivalIndices[i];
                QuickMove(state, index);
            }
            else
            {
                SurvivalClick(state, view.survivalIndices[i], button);
            }
        }
        else if (slot.WasClicked(Pincel::MouseButton::Middle))
        {
            if (survivalClicked) continue;
            survivalClicked = true;
            MiddleClickSurvival(state, view.survivalIndices[i], shiftHeld);
        }
    }

    // Clicking nothing drops the carried stack - but not when the click
    // landed on a widget (tabs, search, rail, grid, destroy), which bubbles
    // up to the backdrop too.
    Element backdrop = ui.Backdrop();
    for (uint8_t button : { Pincel::MouseButton::Left, Pincel::MouseButton::Right })
    {
        if (survivalClicked || pickerClicked || widgetClick || carriedItem.IsEmpty() || !backdrop.WasClicked(button)) continue;
        pickerClicked = true;
        BackdropClick(state, button);
        carriedItem = ItemStack{}; // Re-read below anyway; keep the local copy honest.
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        carriedItem = state->inventory.carriedItem;
    }

    // The destroy slot: plain click clears the cursor, shift+click clears the
    // whole inventory.
    if (view.destroyButton.ClickedButtons() != 0)
    {
        ui.PlayClick();
        DestroyClick(state, shiftHeld);
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        carriedItem = state->inventory.carriedItem;
    }

    // Hotbar number keys and the drop key, on whichever slot is hovered - but
    // not while typing in the search box, where digits are search text.
    bool typing = view.searchInput.IsFocused();
    int hoveredSurvivalIndex = hoveredSurvival >= 0 ? view.survivalIndices[static_cast<size_t>(hoveredSurvival)] : -1;
    int hoveredPickerId = hoveredPicker >= 0 ? view.visibleItemIds[static_cast<size_t>(hoveredPicker)] : -1;

    DropKeyTracker::Result drop = DropKeyTracker::Result::NONE;
    if (!typing)
    {
        drop = view.dropKey.Update(state->input->IsActive("DropItem"), state->input->IsActive("DropItemHold"),
            state->input->IsActive("DropItemRelease"), ImGui::GetIO().KeyCtrl, ImGui::GetIO().DeltaTime);

        for (int32_t hotbarSlot = 0; hotbarSlot < 9; hotbarSlot++)
        {
            if (state->input->WasActivated("Hotbar." + std::to_string(hotbarSlot)))
            {
                HotbarKey(state, hoveredSurvivalIndex, hoveredPickerId, hotbarSlot);
                break;
            }
        }
    }
    else
    {
        view.dropKey.Reset();
    }
    if (drop != DropKeyTracker::Result::NONE && hoveredSurvivalIndex >= 0
        && !ItemAt(snapshot, hoveredSurvivalIndex).IsEmpty())
    {
        DropSurvivalSlot(state, hoveredSurvivalIndex, drop == DropKeyTracker::Result::STACK ? 64 : 1);
    }

    // Refresh the survival panel and cursor after all the mutations above.
    SnapshotWindow0(state, snapshot);
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        carriedItem = state->inventory.carriedItem;
    }
    for (size_t i = 0; i < view.survivalSlots.size(); i++)
        view.survivalSlots[i].Set(ItemAt(snapshot, view.survivalIndices[i]));

    // The carried stack follows the cursor.
    const Pincel::MouseInput& mouse = ui.Mouse();
    view.carried.Set(carriedItem);
    view.cursor.SetVisible(!carriedItem.IsEmpty() && !std::isnan(mouse.x));
    if (!std::isnan(mouse.x))
    {
        Pincel::Insets at;
        at.left = static_cast<int>(mouse.x) - slotPx / 2;
        at.top = static_cast<int>(mouse.y) - slotPx / 2;
        view.cursor.SetInsets(at);
    }

    // The tooltip of the hovered stack (not while carrying one, as in vanilla).
    ItemStack hoveredItem{};
    if (hoveredPicker >= 0) hoveredItem = ItemStack{ hoveredPickerId, 1 };
    else if (hoveredSurvival >= 0) hoveredItem = ItemAt(snapshot, view.survivalIndices[static_cast<size_t>(hoveredSurvival)]);
    bool showTooltip = (hoveredPicker >= 0 || hoveredSurvival >= 0) && carriedItem.IsEmpty()
        && !hoveredItem.IsEmpty() && !std::isnan(mouse.x);
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
    int x = static_cast<int>(mouse.x) + Px(ui, 12.0f);
    int y = static_cast<int>(mouse.y) - Px(ui, 12.0f);
    x = std::clamp(x, 0, std::max(0, ui.Width() - size.w));
    y = std::clamp(y, 0, std::max(0, ui.Height() - size.h));
    Pincel::Insets at;
    at.left = x;
    at.top = y;
    view.tooltip.SetInsets(at);
}

} // namespace

Screen* CreateCreativeScreen(GlobalState* state)
{
    Screen* screen = GUIController::CreateScreen("Creative Inventory", /* closable */ true);
    screen->showTitle = false; // The layout carries its own.
    screen->fontSize = Theme::SMALL_FONT_SIZE;
    screen->fontInArtPixels = true;

    auto view = std::make_shared<CreativeView>();
    screen->build = [state, screen, view](ControlRenderer& ui) { BuildCreative(ui, state, *screen, *view); };
    screen->update = [state, screen, view](ControlRenderer& ui) { UpdateCreative(ui, state, *screen, *view); };
    return screen;
}

} // namespace Volcano::GUI
