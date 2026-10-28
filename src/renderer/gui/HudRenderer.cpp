#include "HudRenderer.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <imgui.h>
#include "GlobalState.hpp"
#include "InputHandler.hpp"
#include "Logger.hpp"
#include "PlayerAttributes.hpp"
#include "TickLoop.hpp"
#include "ControlRenderer.hpp"
#include "PincelSprite.hpp"
#include "Theme.hpp"
#include "models/Chat.hpp"
#include "../VulkanInit.hpp"

namespace Volcano::GUI {

namespace {

using Pincel::Color;
using Pincel::Element;
using Pincel::Orientation;
using Pincel::PointerEvents;
using Pincel::Position;
using namespace Theme;

Color TokenColor(const CommandToken& token)
{
    switch (token.kind)
    {
    case CommandToken::Kind::Literal: return LABEL_TEXT;
    case CommandToken::Kind::Partial: return TEXT_DIM;
    case CommandToken::Kind::Invalid: return Color{ 255, 85, 85 };
    case CommandToken::Kind::Argument: break;
    }
    switch (token.parser)
    {
    case CommandParser::Integer:
    case CommandParser::Long:
    case CommandParser::Float:
    case CommandParser::Double:
    case CommandParser::Time:
    case CommandParser::Angle: return ORANGE;
    case CommandParser::Entity:
    case CommandParser::GameProfile:
    case CommandParser::ScoreHolder:
    case CommandParser::Team: return Color{ 89, 235, 255 };
    case CommandParser::BlockPos:
    case CommandParser::ColumnPos:
    case CommandParser::Vec2:
    case CommandParser::Vec3:
    case CommandParser::Rotation: return Color{ 128, 255, 32 };
    default: return Color{ 255, 236, 128 };
    }
}

// Vanilla's HUD geometry, in texture pixels; Art() turns them into screen pixels.
constexpr float HOTBAR_WIDTH = 182.0f;
constexpr float HOTBAR_HEIGHT = 22.0f;
constexpr float HOTBAR_SLOT_STRIDE = 20.0f;
constexpr float ITEM_SIZE = 16.0f;
constexpr float HUD_BOTTOM_MARGIN = 2.0f;
constexpr float ICON_SIZE = 9.0f;
constexpr float ICON_STRIDE = 8.0f;
constexpr float ROW_HEIGHT = 10.0f;
constexpr float ROW_OFFSET = 17.0f;    // Icon top above the hotbar's top.
constexpr float EXPERIENCE_HEIGHT = 5.0f;
constexpr float EXPERIENCE_OFFSET = 7.0f;

// In UI units, which Units() scales.
constexpr float PANEL_GAP = 10.0f;
constexpr float MOVEMENT_WIDTH = 140.0f;
constexpr float CHAT_WIDTH = 480.0f;
constexpr float CHAT_HEIGHT = 220.0f;
constexpr float CHAT_MARGIN = 8.0f;
constexpr float CHAT_GAP = 4.0f;
constexpr float CHAT_TEXT_SIZE = 12.0f;
constexpr float CHAT_PAD_Y = 6.0f;
constexpr size_t CHAT_HISTORY_MAX = 100;
constexpr float CHAT_PAGE_FRACTION = 0.8f;
constexpr float CHAT_BADGE_WIDTH = 22.0f;   // The "/" slot at the left of the box in command mode.
constexpr float CHAT_TOOLTIP_WIDTH = 440.0f;
constexpr float CHAT_LIST_WIDTH = 150.0f;
constexpr size_t CHAT_QUEUE_SHOWN = 5;
constexpr size_t CHAT_SUGGESTIONS_SHOWN = 8;
constexpr double SEND_INTERVAL = 0.05;      // Seconds between queued lines, a server tick.
constexpr const char* CHAT_WRAP_MARKER = "\xE2\x86\x92"; // An arrow, at the start of a command's wrapped rows.
const Color CHAT_LOG_FILL{ 54, 54, 54, 0.9f };
constexpr float FRAME_BORDER_SIZE = 2.0f;
constexpr float FRAME_RADIUS = 8.0f;
constexpr float FIELD_PADDING = 8.0f;

constexpr const char* HUD_ART = "assets/minecraft/textures/gui/sprites/hud/";

// AppleSkin's saturation outline: the hunger icon's black outline in gold, the rest clear.
Sprite OutlineOf(const Sprite& source)
{
    Sprite outline = source;
    for (size_t i = 0; i + 3 < outline.rgba.size(); i += 4)
    {
        uint8_t* pixel = outline.rgba.data() + i;
        bool edge = pixel[3] > 0 && pixel[0] < 24 && pixel[1] < 24 && pixel[2] < 24;
        pixel[0] = 255;
        pixel[1] = 205;
        pixel[2] = 40;
        pixel[3] = edge ? 255 : 0;
    }
    return outline;
}

} // namespace

int HudRenderer::Art(float pixels) const
{
    return static_cast<int>(std::lround(pixels * static_cast<float>(scale + 1)));
}

// A font's pixels are drawn one art pixel at vanilla's scale (GUI scale + 1), which images no longer use.
int HudRenderer::FontPixels(float pixels) const
{
    return static_cast<int>(std::lround(pixels * static_cast<float>(scale + 1)));
}

int HudRenderer::Units(float units) const
{
    return static_cast<int>(std::lround(units * static_cast<float>(scale)));
}

bool HudRenderer::Init(VkRenderPass renderPass)
{
    Pincel::Init();
    if (!surface.Create(renderPass)) return false;

    canvas = std::make_unique<Pincel::Canvas>();
    canvas->SetBackground(Color{ 0, 0, 0, 0.0f });
    canvas->SetForeground(LABEL_TEXT);
    canvas->SetClipboard(
        [] { const char* text = ImGui::GetClipboardText(); return std::string(text != nullptr ? text : ""); },
        [](const std::string& text) { ImGui::SetClipboardText(text.c_str()); });
    canvas->Root().SetFontFamily(ControlRenderer::UiFont());
    commandHelp.Load("resources/commands.json");

    // The chat's fonts borrow what they lack (the death icon, symbols) from the pixel font and its system fallbacks.
    Pincel::FontFamily uiFont = ControlRenderer::UiFont();
    if (!(uiFont == Pincel::FontFamily::Sans))
    {
        Pincel::SetFontFallback(Pincel::FontFamily::Sans, uiFont);
        Pincel::SetFontFallback(Pincel::FontFamily::Mono, uiFont);
    }

    ready = true;
    return true;
}

void HudRenderer::Shutdown()
{
    ready = false;
    canvas.reset();
    surface.Destroy();
}

void HudRenderer::ReloadResources()
{
    Sprite::ClearCache();
    ItemSlotView::ClearCache();
    scale = 0; // Builds again next frame.
}

// ---- Building ----

Element HudRenderer::Place(int left, int top, int boxWidth, int boxHeight)
{
    Pincel::Insets insets;
    insets.left = left;
    insets.top = top;
    Element box = canvas->Root().AddStack(Orientation::Vertical);
    box.SetPosition(Position::Fixed)
        .SetInsets(insets)
        .SetSize(static_cast<uint16_t>(boxWidth), static_cast<uint16_t>(boxHeight))
        .SetPointerEvents(PointerEvents::None);
    return box;
}

Element HudRenderer::Corner(Element element, int left, int top)
{
    Pincel::Insets insets;
    insets.left = left;
    insets.top = top;
    element.SetPosition(Position::Absolute).SetInsets(insets).SetPointerEvents(PointerEvents::None);
    return element;
}

// An art sprite scaled to `art` pixels per texture pixel, placed at (left, top) inside `parent`.
// A cropWidth above 0 keeps only that many columns from the left.
Element HudRenderer::AddSprite(Element parent, const std::string& path, int logicalWidth, int logicalHeight,
    int left, int top, int art, int cropWidth)
{
    const Sprite& sprite = Sprite::Cached(HUD_ART + path);
    auto image = sprite.Region(logicalWidth, logicalHeight, 0, 0, cropWidth > 0 ? cropWidth : logicalWidth, logicalHeight, art).AddTo(parent);
    return image ? Corner(*image, left, top) : Element();
}

void HudRenderer::Build()
{
    int art = scale + 1;
    Element root = canvas->Root();
    root.RemoveChildren();
    root.SetFontSize(static_cast<uint16_t>(FontPixels(SMALL_FONT_SIZE)));
    canvas->SetTextShadow(SHADOW, FontPixels(1.0f), FontPixels(1.0f));

    int hotbarLeft = (static_cast<int>(width) - Art(HOTBAR_WIDTH)) / 2;
    int hotbarTop = static_cast<int>(height) - Art(HOTBAR_HEIGHT) - Art(HUD_BOTTOM_MARGIN);
    int fontPixels = FontPixels(SMALL_FONT_SIZE);

    // Hotbar: the art, the nine slots, and the frame on the held one.
    Element hotbar = Place(hotbarLeft, hotbarTop, Art(HOTBAR_WIDTH), Art(HOTBAR_HEIGHT));
    AddSprite(hotbar, "hotbar.png", 182, 22, 0, 0, art);
    for (int i = 0; i < HOTBAR_SLOTS; i++)
    {
        hotbarSlots[static_cast<size_t>(i)] = ItemSlotView::Add(hotbar,
            Art(3.0f + static_cast<float>(i) * HOTBAR_SLOT_STRIDE), Art(3.0f), Art(ITEM_SIZE), fontPixels);
    }
    selection = AddSprite(hotbar, "hotbar_selection.png", 24, 23, Art(-1.0f), Art(-1.0f), art);

    // Armor: the hotbar art cut down to four slots (its first 81 columns and the 1 column border).
    int armorWidth = ARMOR_SLOTS * static_cast<int>(HOTBAR_SLOT_STRIDE) + 2;
    Element armor = Place(hotbarLeft - Units(PANEL_GAP) - Art(static_cast<float>(armorWidth)), hotbarTop,
        Art(static_cast<float>(armorWidth)), Art(HOTBAR_HEIGHT));
    Sprite hotbarSprite = Sprite::Cached(std::string(HUD_ART) + "hotbar.png");
    if (auto body = hotbarSprite.Region(182, 22, 0, 0, armorWidth - 1, 22, art).AddTo(armor))
    {
        Corner(*body, 0, 0);
    }
    if (auto edge = hotbarSprite.Region(182, 22, 181, 0, 1, 22, art).AddTo(armor))
    {
        Corner(*edge, Art(static_cast<float>(armorWidth - 1)), 0);
    }
    for (int i = 0; i < ARMOR_SLOTS; i++)
    {
        armorSlots[static_cast<size_t>(i)] = ItemSlotView::Add(armor,
            Art(3.0f + static_cast<float>(i) * HOTBAR_SLOT_STRIDE), Art(3.0f), Art(ITEM_SIZE), fontPixels);
    }

    // Hearts, hunger and armor above the hotbar.
    Element status = Place(hotbarLeft, hotbarTop, Art(HOTBAR_WIDTH), Art(HOTBAR_HEIGHT));
    BuildStatus(status, art);

    // Experience bar between the hotbar and the icons, and the level above it.
    Element experience = Place(hotbarLeft, hotbarTop - Art(EXPERIENCE_OFFSET), Art(HOTBAR_WIDTH), Art(EXPERIENCE_HEIGHT));
    AddSprite(experience, "experience_bar_background.png", 182, 5, 0, 0, art);
    experienceFill = AddSprite(experience, "experience_bar_progress.png", 182, 5, 0, 0, art);
    shownExperienceWidth = -1;
    Pincel::Insets above;
    above.bottom = Art(EXPERIENCE_HEIGHT + 1.0f);
    above.centerX = true;
    levelText = experience.AddText("");
    levelText.SetPosition(Position::Absolute)
        .SetInsets(above)
        .SetForeground(Color{ 128, 255, 32 })
        .SetPointerEvents(PointerEvents::None)
        .SetVisible(false);
    shownLevel = -1;

    // The movement state, right of the hotbar.
    Element movement = Place(hotbarLeft + Art(HOTBAR_WIDTH) + Units(PANEL_GAP), hotbarTop, Units(MOVEMENT_WIDTH), Art(HOTBAR_HEIGHT));
    movement.SetAlign(Pincel::Align::Center, Pincel::Align::Center)
        .SetBackground(Color{ PANEL_DARK.r, PANEL_DARK.g, PANEL_DARK.b, 0.96f })
        .SetBorder(static_cast<uint16_t>(Units(FRAME_BORDER_SIZE)), PANEL_LIGHT)
        .SetCornerRadius(static_cast<uint16_t>(Units(FRAME_RADIUS)));
    movementText = movement.AddText("Idle");
    movementText.SetForeground(Color{ 89, 235, 255 });

    // Chat: the log at the top left, and under it the box while typing, with its popup anchored to the word.
    uint16_t border = static_cast<uint16_t>(Units(FRAME_BORDER_SIZE));
    uint16_t padding = static_cast<uint16_t>(Units(FIELD_PADDING));
    uint16_t radius = static_cast<uint16_t>(Units(FRAME_RADIUS));
    uint16_t chatText = static_cast<uint16_t>(Units(CHAT_TEXT_SIZE));
    Color noShadow{ 0, 0, 0, 0.0f };

    chatBox = Place(Units(CHAT_MARGIN), Units(CHAT_MARGIN), Units(CHAT_WIDTH), Units(CHAT_HEIGHT));
    chatBox.SetFontFamily(Pincel::FontFamily::Sans)
        .SetFontSize(chatText)
        .SetTextShadow(noShadow, 0, 0)
        .SetBackground(CHAT_LOG_FILL)
        .SetBorder(border, PANEL_DARK)
        .SetCornerRadius(radius)
        .SetPadding(padding)
        .SetOverflow(Pincel::Overflow::Scroll)
        .SetStickToEnd(true);
    chatLog = chatBox;
    chatRevision = ~0ull;

    // The box grows with the command, so it and the queued lines under it are stacked in a column.
    chatColumn = Place(Units(CHAT_MARGIN), Units(CHAT_MARGIN + CHAT_HEIGHT + CHAT_GAP), Units(CHAT_WIDTH), 0);
    chatColumn.SetGap(static_cast<uint16_t>(Units(CHAT_GAP)));
    Element typing = chatColumn.AddStack(Orientation::Vertical);
    typing.SetSize(static_cast<uint16_t>(Units(CHAT_WIDTH)), 0)
        .SetFontFamily(Pincel::FontFamily::Mono)
        .SetFontSize(chatText)
        .SetTextShadow(noShadow, 0, 0)
        .SetPointerEvents(PointerEvents::Auto)
        .SetAlign(Pincel::Align::Start, Pincel::Align::Start)
        .SetBackground(PANEL_MID)
        .SetBorder(border, PANEL_DARK)
        .SetCornerRadius(radius)
        .SetVisible(false);
    chatInput = typing.AddInput(Pincel::InputKind::Text);
    chatInput.SetInputWrap(true, CHAT_WRAP_MARKER);
    chatTyping = typing;

    // The "/" badge sits in the padding left of the text, on the first row, and shows in command mode.
    Pincel::Insets badgeAt;
    badgeAt.left = 0;
    badgeAt.top = Units(CHAT_PAD_Y);
    chatBadge = typing.AddStack(Orientation::Vertical);
    chatBadge.SetPosition(Position::Absolute)
        .SetInsets(badgeAt)
        .SetSize(static_cast<uint16_t>(Units(CHAT_BADGE_WIDTH)), 0)
        .SetAlign(Pincel::Align::Center, Pincel::Align::Start)
        .SetPointerEvents(PointerEvents::None);
    chatBadge.AddText("/").SetForeground(ORANGE);
    SetCommandMode(commandMode);

    // Lines set aside with Shift+Enter, under the box.
    chatQueueBox = chatColumn.AddStack(Orientation::Vertical);
    chatQueueBox.SetFontFamily(Pincel::FontFamily::Sans)
        .SetFontSize(chatText)
        .SetTextShadow(noShadow, 0, 0)
        .SetVisible(false);

    // The popup is its own piece of the canvas, so it can sit anywhere under the box.
    chatPopup = Place(0, 0, 0, 0);
    chatPopup.SetGap(static_cast<uint16_t>(Units(CHAT_GAP)))
        .SetFontFamily(Pincel::FontFamily::Sans)
        .SetFontSize(chatText)
        .SetTextShadow(noShadow, 0, 0)
        .SetVisible(false);
    queueDirty = true;
}

// Shows or hides the badge, and makes room for it in the box.
void HudRenderer::SetCommandMode(bool on)
{
    commandMode = on;
    uint16_t border = static_cast<uint16_t>(Units(FRAME_BORDER_SIZE));
    uint16_t padding = static_cast<uint16_t>(Units(FIELD_PADDING));
    uint16_t paddingY = static_cast<uint16_t>(Units(CHAT_PAD_Y));
    int badge = on ? Units(CHAT_BADGE_WIDTH) : 0;
    chatTyping.SetPadding(paddingY, padding, paddingY, static_cast<uint16_t>(padding + badge));
    chatInput.SetSize(static_cast<uint16_t>(std::max(1, Units(CHAT_WIDTH) - 2 * (border + padding) - badge)), 0);
    chatBadge.SetVisible(on);
}

// What the box holds as it would be sent, with the command's "/" put back.
std::string HudRenderer::ComposeChat() const
{
    return (commandMode ? "/" : "") + chatInput.GetText();
}

// Puts a line in the box, entering command mode for a "/" one.
void HudRenderer::LoadChat(const std::string& text)
{
    bool command = !text.empty() && text[0] == '/';
    SetCommandMode(command);
    chatInput.SetText(command ? text.substr(1) : text);
}

// Reads `text` (a command, without its "/") against the server's tree.
void HudRenderer::AnalyzeChat(const std::string& text)
{
    {
        std::lock_guard<std::mutex> lock(state->commandsMutex);
        analyzedTree = state->commands;
    }
    std::vector<std::string> players;
    {
        std::lock_guard<std::mutex> lock(state->playerListMutex);
        for (const auto& entry : state->playerList) players.push_back(entry.second);
    }
    std::sort(players.begin(), players.end());

    analysis = analyzedTree ? CommandWalker(analyzedTree).Analyze(text, players) : CommandAnalysis{};
    analyzedText = text;
    analyzed = true;
}

// Tab writes the next suggestion over the word being typed, and Tab again the one after it.
void HudRenderer::CompleteChat(bool backwards)
{
    std::string text = chatInput.GetText();
    if (cycleIndex < 0 || text != cycleApplied)
    {
        if (!analyzed || text != analyzedText) AnalyzeChat(text);
        cycleList.clear();
        for (const CommandSuggestion& suggestion : analysis.suggestions)
        {
            if (!suggestion.hint) cycleList.push_back(suggestion);
        }
        if (cycleList.empty()) return;
        cycleBase = text.substr(0, analysis.partialStart);
        cycleIndex = -1;
    }

    int count = static_cast<int>(cycleList.size());
    cycleIndex = backwards ? (cycleIndex <= 0 ? count - 1 : cycleIndex - 1) : (cycleIndex + 1) % count;
    const CommandSuggestion& pick = cycleList[static_cast<size_t>(cycleIndex)];
    cycleApplied = cycleBase + pick.text + (pick.more ? " " : "");
    chatInput.SetText(cycleApplied);
}

void HudRenderer::RefreshChatPopup(bool open)
{
    // The command's reading goes stale when the text, the tree or the cycling moves.
    bool command = open && commandMode;
    if (command)
    {
        std::string text = chatInput.GetText();
        std::shared_ptr<const CommandTree> tree;
        {
            std::lock_guard<std::mutex> lock(state->commandsMutex);
            tree = state->commands;
        }
        int cycle = cycleIndex >= 0 && text == cycleApplied ? cycleIndex : -1;
        if (!analyzed || text != analyzedText || tree != analyzedTree || cycle != shownCycle)
        {
            AnalyzeChat(text);
            shownCycle = cycle;
            chatInput.SetInputHighlights(commandStyle.Highlight(analyzedText, analysis));
            commandDescription = commandHelp.Describe(commandStyle.Words(analyzedText, analysis));
            queueDirty = true;
        }
    }
    else if (analyzed)
    {
        analysis = CommandAnalysis{};
        analyzedTree.reset();
        analyzed = false;
        commandDescription.clear();
        chatInput.SetInputHighlights({});
        queueDirty = true;
    }

    if (queueDirty)
    {
        queueDirty = false;
        RebuildChatPopup();
    }
    chatQueueBox.SetVisible(open && !chatQueue.empty());
    PlaceChatPopup(open);
}

// The lines set aside, and the popup: a tooltip with what the command does and its usage, and the suggestions.
void HudRenderer::RebuildChatPopup()
{
    uint16_t border = static_cast<uint16_t>(Units(FRAME_BORDER_SIZE));
    uint16_t padding = static_cast<uint16_t>(Units(FIELD_PADDING));
    uint16_t radius = static_cast<uint16_t>(Units(FRAME_RADIUS));
    uint16_t small = static_cast<uint16_t>(Units(CHAT_TEXT_SIZE * 0.75f));
    Color shade{ PANEL_DARK.r, PANEL_DARK.g, PANEL_DARK.b, 0.7f };

    chatQueueBox.RemoveChildren();
    size_t queued = chatQueue.size() > CHAT_QUEUE_SHOWN ? chatQueue.size() - CHAT_QUEUE_SHOWN : 0;
    if (queued > 0) chatQueueBox.AddText("+" + std::to_string(queued) + " queued").SetForeground(Syntax::DIM);
    for (size_t i = queued; i < chatQueue.size(); i++)
    {
        Element line = chatQueueBox.AddText(chatQueue[i]);
        line.SetForeground(chatQueue[i][0] == '/' ? Syntax::COMMAND : Syntax::PLAIN).SetBackground(shade);
    }

    chatPopup.RemoveChildren();
    popupHasContent = false;
    if (!analyzed || !analyzedTree) return;

    bool hasUsage = !analysis.usage.empty();
    if (!commandDescription.empty() || hasUsage)
    {
        Element tip = chatPopup.AddStack(Orientation::Vertical);
        tip.SetSize(static_cast<uint16_t>(Units(CHAT_TOOLTIP_WIDTH)), 0)
            .SetPadding(padding)
            .SetGap(static_cast<uint16_t>(Units(CHAT_GAP)))
            .SetBackground(PANEL_MID)
            .SetBorder(border, PANEL_DARK)
            .SetCornerRadius(radius);
        if (!commandDescription.empty()) tip.AddText(commandDescription).SetForeground(Syntax::PLAIN);
        if (hasUsage)
        {
            Element usage = tip.AddRichText(commandStyle.UsageSpans(analysis));
            usage.SetFontFamily(Pincel::FontFamily::Mono);
        }
        popupHasContent = true;
    }

    // What Tab offers, with the one it takes next marked.
    std::vector<const CommandSuggestion*> values;
    const std::vector<CommandSuggestion>& source = shownCycle >= 0 ? cycleList : analysis.suggestions;
    for (const CommandSuggestion& suggestion : source)
    {
        if (!suggestion.hint) values.push_back(&suggestion);
    }
    if (values.empty()) return;

    Element list = chatPopup.AddStack(Orientation::Vertical);
    list.SetSize(static_cast<uint16_t>(Units(CHAT_LIST_WIDTH)), 0)
        .SetPadding(static_cast<uint16_t>(Units(CHAT_GAP)))
        .SetBackground(PANEL_MID)
        .SetBorder(border, PANEL_DARK)
        .SetCornerRadius(radius);
    size_t selected = shownCycle >= 0 ? static_cast<size_t>(shownCycle) : 0;
    size_t first = selected >= CHAT_SUGGESTIONS_SHOWN ? selected + 1 - CHAT_SUGGESTIONS_SHOWN : 0;
    for (size_t i = first; i < values.size() && i < first + CHAT_SUGGESTIONS_SHOWN; i++)
    {
        bool picked = i == selected;
        Element row = list.AddStack(Orientation::Horizontal);
        row.SetAlign(Pincel::Align::Start, Pincel::Align::Center)
            .SetGap(static_cast<uint16_t>(Units(6.0f)))
            .SetPadding(static_cast<uint16_t>(Units(3.0f)), static_cast<uint16_t>(Units(6.0f)), static_cast<uint16_t>(Units(3.0f)), static_cast<uint16_t>(Units(6.0f)));
        if (picked) row.SetBackground(Color{ 0x85, 0x57, 0x0D }).SetCornerRadius(static_cast<uint16_t>(Units(4.0f)));
        bool literal = values[i]->literal;
        row.AddText(literal ? ">" : "{}").SetForeground(literal ? Syntax::CONST_PARAMETER : Syntax::OPERATOR);
        row.AddText(values[i]->label).SetForeground(picked ? Color{ 255, 255, 255 } : Syntax::PLAIN);
        if (picked)
        {
            Element chip = row.AddStack(Orientation::Vertical);
            chip.SetPadding(static_cast<uint16_t>(Units(1.0f)), static_cast<uint16_t>(Units(4.0f)), static_cast<uint16_t>(Units(1.0f)), static_cast<uint16_t>(Units(4.0f)))
                .SetBackground(Color{ 0x2A, 0x1D, 0x08 })
                .SetBorder(static_cast<uint16_t>(Units(1.0f)), Syntax::NUMBER)
                .SetCornerRadius(static_cast<uint16_t>(Units(3.0f)));
            chip.AddText("TAB").SetFontSize(small).SetForeground(Syntax::NUMBER);
        }
    }
    if (values.size() > first + CHAT_SUGGESTIONS_SHOWN)
    {
        list.AddText("+" + std::to_string(values.size() - first - CHAT_SUGGESTIONS_SHOWN) + " more").SetForeground(Syntax::DIM);
    }
    popupHasContent = true;
}

// Puts the popup under the box, at the word being typed, and keeps it on the canvas.
void HudRenderer::PlaceChatPopup(bool open)
{
    bool show = open && commandMode && popupHasContent;
    chatPopup.SetVisible(show);
    if (!show) return;

    Element::Rect typingRect = chatTyping.GetRect();
    int bottom = typingRect.y + typingRect.h;
    if (!chatQueue.empty())
    {
        Element::Rect queueRect = chatQueueBox.GetRect();
        bottom = std::max(bottom, queueRect.y + queueRect.h);
    }

    // The word starts at this character, so the popup lines up with it, wherever the text wrapped to.
    size_t chars = 0;
    for (size_t i = 0; i < analysis.partialStart && i < analyzedText.size(); i++)
    {
        if ((static_cast<unsigned char>(analyzedText[i]) & 0xC0) != 0x80) chars++;
    }
    int x = typingRect.x;
    if (std::optional<Element::Rect> at = chatInput.GetInputPosition(chars)) x = at->x;

    int mostX = static_cast<int>(width) - chatPopup.GetRect().w - Units(CHAT_MARGIN);
    Pincel::Insets place;
    place.left = std::max(Units(CHAT_MARGIN), std::min(x, mostX));
    place.top = bottom + Units(CHAT_GAP);
    chatPopup.SetInsets(place);
}

void HudRenderer::BuildStatus(Element parent, int art)
{
    float maxHealth = static_cast<float>(state->attributes->GetDouble("generic.max_health", 20.0));
    builtHearts = std::max(1, static_cast<int>(std::ceil(maxHealth * 0.5f)));

    hearts.clear();
    for (int i = 0; i < builtHearts; i++)
    {
        int x = Art(static_cast<float>(i % ICONS_PER_ROW) * ICON_STRIDE);
        int y = -Art(ROW_OFFSET + static_cast<float>(i / ICONS_PER_ROW) * ROW_HEIGHT);
        AddSprite(parent, "heart/container.png", 9, 9, x, y, art);
        HeartIcon heart;
        heart.full = AddSprite(parent, "heart/full.png", 9, 9, x, y, art);
        heart.half = AddSprite(parent, "heart/half.png", 9, 9, x, y, art);
        hearts.push_back(heart);
    }

    Sprite outline = OutlineOf(Sprite::Cached(std::string(HUD_ART) + "food_empty.png"));
    for (int i = 0; i < ICONS_PER_ROW; i++)
    {
        int x = Art(HOTBAR_WIDTH - ICON_SIZE - static_cast<float>(i) * ICON_STRIDE);
        int y = -Art(ROW_OFFSET);
        FoodIcon& icon = food[static_cast<size_t>(i)];
        AddSprite(parent, "food_empty.png", 9, 9, x, y, art);
        icon.full = AddSprite(parent, "food_full.png", 9, 9, x, y, art);
        icon.half = AddSprite(parent, "food_half.png", 9, 9, x, y, art);
        if (auto whole = outline.Region(9, 9, 0, 0, 9, 9, art).AddTo(parent)) icon.saturation = Corner(*whole, x, y);
        if (auto half = outline.Region(9, 9, 0, 0, 5, 9, art).AddTo(parent))
        {
            icon.halfSaturation = Corner(*half, x, y);
        }
    }

    // The armor row sits above the topmost row of hearts.
    int heartRows = (builtHearts + ICONS_PER_ROW - 1) / ICONS_PER_ROW;
    int armorTop = -Art(ROW_OFFSET + static_cast<float>(heartRows) * ROW_HEIGHT);
    armorRow = parent.AddStack(Orientation::Vertical);
    Corner(armorRow, 0, armorTop).SetSize(static_cast<uint16_t>(Art(ICON_STRIDE * ICONS_PER_ROW)), static_cast<uint16_t>(Art(ICON_SIZE)));
    for (int i = 0; i < ICONS_PER_ROW; i++)
    {
        int x = Art(static_cast<float>(i) * ICON_STRIDE);
        ArmorIcon& icon = armorIcons[static_cast<size_t>(i)];
        AddSprite(armorRow, "armor_empty.png", 9, 9, x, 0, art);
        icon.full = AddSprite(armorRow, "armor_full.png", 9, 9, x, 0, art);
        icon.half = AddSprite(armorRow, "armor_half.png", 9, 9, x, 0, art);
    }
}

// ---- Per frame ----

void HudRenderer::RefreshStatus()
{
    float health, saturation;
    int32_t hunger;
    {
        std::lock_guard<std::mutex> lock(state->healthMutex);
        health = state->health;
        hunger = state->food;
        saturation = state->saturation;
    }
    for (size_t i = 0; i < hearts.size(); i++)
    {
        float point = static_cast<float>(i * 2);
        hearts[i].full.SetVisible(health >= point + 2.0f);
        hearts[i].half.SetVisible(health >= point + 1.0f && health < point + 2.0f);
    }
    for (int i = 0; i < ICONS_PER_ROW; i++)
    {
        FoodIcon& icon = food[static_cast<size_t>(i)];
        icon.full.SetVisible(hunger >= i * 2 + 2);
        icon.half.SetVisible(hunger == i * 2 + 1);
        float points = saturation - static_cast<float>(i * 2);
        icon.saturation.SetVisible(points >= 2.0f);
        icon.halfSaturation.SetVisible(points >= 1.0f && points < 2.0f);
    }

    int armor = static_cast<int>(state->attributes->GetDouble("generic.armor", 0.0));
    armorRow.SetVisible(armor > 0);
    for (int i = 0; i < ICONS_PER_ROW; i++)
    {
        ArmorIcon& icon = armorIcons[static_cast<size_t>(i)];
        icon.full.SetVisible(armor >= i * 2 + 2);
        icon.half.SetVisible(armor == i * 2 + 1);
    }

    float progress;
    int32_t level;
    {
        std::lock_guard<std::mutex> lock(state->experienceMutex);
        progress = std::clamp(state->experienceProgress, 0.0f, 1.0f);
        level = state->experienceLevel;
    }
    // Whole texels only, as in vanilla.
    int filled = static_cast<int>(std::floor(progress * HOTBAR_WIDTH));
    if (filled != shownExperienceWidth)
    {
        shownExperienceWidth = filled;
        Sprite fill = Sprite::Cached(std::string(HUD_ART) + "experience_bar_progress.png");
        if (filled > 0) fill.Region(182, 5, 0, 0, filled, 5, scale + 1).SetOn(experienceFill);
        experienceFill.SetVisible(filled > 0);
    }
    if (level != shownLevel)
    {
        shownLevel = level;
        levelText.SetText(std::to_string(level));
        levelText.SetVisible(level > 0);
    }
}

// Priority Flying > Swimming > Sneaking > Sprinting > Walking, showing the highest that applies.
void HudRenderer::RefreshMovement()
{
    TickLoop::MovementSnapshot movement = state->tickLoop->GetMovementSnapshot();
    bool swimming = movement.mode == MoveMode::Swim || movement.mode == MoveMode::SwimSprint;
    bool sneaking = state->input->IsActive("Sneak");
    bool moving = std::fabs(state->input->GetAxis("Move.Forward")) > 0.01f
        || std::fabs(state->input->GetAxis("Move.Right")) > 0.01f;

    const char* label = "Idle";
    if (swimming) label = "Swimming";
    else if (sneaking) label = "Sneaking";
    else if (movement.sprinting) label = "Sprinting";
    else if (moving) label = "Walking";
    movementText.SetText(label);
}

void HudRenderer::RefreshChat(const ChatBox& chat)
{
    uint64_t revision = Chat::GetRevision();
    if (revision != chatRevision)
    {
        chatRevision = revision;
        chatLog.RemoveChildren();
        for (const ChatLine& line : Chat::GetLines())
        {
            std::vector<Pincel::TextSpan> spans;
            for (const ChatSegment& segment : line.segments)
            {
                Pincel::TextSpan span;
                span.text = segment.text;
                span.color = Color{ static_cast<uint8_t>((segment.colorRGB >> 16) & 0xFF),
                    static_cast<uint8_t>((segment.colorRGB >> 8) & 0xFF), static_cast<uint8_t>(segment.colorRGB & 0xFF) };
                spans.push_back(std::move(span));
            }
            if (!spans.empty()) chatLog.AddRichText(spans);
        }
    }

    chatTyping.SetVisible(chat.open);
    if (chat.justOpened)
    {
        // The key that opened the box is still queued as typed text, and would land in it.
        ImGui::GetIO().InputQueueCharacters.resize(0);
        chatQueue.clear();
        queueDirty = true;
        cycleIndex = -1;
        LoadChat(chat.initialText);
        chatInput.Focus();
        historyPosition = chatHistory.size();
    }
    else if (!chat.open && chatInput.IsFocused())
    {
        chatInput.Blur();
    }
    else if (chat.open)
    {
        RefreshChatNavigation();
    }

    if (!chat.open && !chatQueue.empty())
    {
        chatQueue.clear();
        queueDirty = true;
    }
    RefreshChatPopup(chat.open);
}

// Up and Down walk the messages sent before (the half-typed one comes back at the bottom),
// Page Up and Page Down scroll the log. Pincel's single-line input ignores all four.
void HudRenderer::RefreshChatNavigation()
{
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && historyPosition > 0)
    {
        if (historyPosition == chatHistory.size()) historyDraft = ComposeChat();
        LoadChat(chatHistory[--historyPosition]);
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && historyPosition < chatHistory.size())
    {
        historyPosition++;
        LoadChat(historyPosition == chatHistory.size() ? historyDraft : chatHistory[historyPosition]);
    }

    if (commandMode && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) CompleteChat(ImGui::GetIO().KeyShift);

    // Backspace that changes nothing was at the start of the text: it takes the badge away.
    backspaceWatch = commandMode && ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && !chatInput.GetSelection();
    if (backspaceWatch) backspaceText = chatInput.GetText();

    int page = static_cast<int>(static_cast<float>(Units(CHAT_HEIGHT)) * CHAT_PAGE_FRACTION);
    int direction = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) direction = -1;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) direction = 1;
    if (direction != 0)
    {
        Pincel::ScrollState scroll = chatLog.GetScroll();
        chatLog.SetScroll(0, std::clamp(scroll.y + direction * page, 0, scroll.maxY));
    }
}

void HudRenderer::RememberChat(const std::string& text)
{
    if (!text.empty() && (chatHistory.empty() || chatHistory.back() != text)) chatHistory.push_back(text);
    if (chatHistory.size() > CHAT_HISTORY_MAX) chatHistory.erase(chatHistory.begin());
    historyPosition = chatHistory.size();
    historyDraft.clear();
}

void HudRenderer::Refresh()
{
    int32_t selected;
    std::array<ItemStack, HOTBAR_SLOTS> hotbarItems;
    std::array<ItemStack, ARMOR_SLOTS> armorItems;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        selected = state->inventory.selectedHotbarSlot;
        for (size_t i = 0; i < hotbarItems.size(); i++) hotbarItems[i] = state->inventory.hotbar.Get(i);
        for (size_t i = 0; i < armorItems.size(); i++) armorItems[i] = state->inventory.armor.Get(i);
    }
    for (size_t i = 0; i < hotbarItems.size(); i++) hotbarSlots[i].Set(hotbarItems[i]);
    for (size_t i = 0; i < armorItems.size(); i++) armorSlots[i].Set(armorItems[i]);

    // The frame slides to the held slot.
    float slot = animations.Track("hotbar.selection", static_cast<float>(selected), static_cast<float>(ImGui::GetTime()),
        0.2f, Easing::CssEaseOut);
    Pincel::Insets at;
    at.left = Art(-1.0f + slot * HOTBAR_SLOT_STRIDE);
    at.top = Art(-1.0f);
    selection.SetInsets(at);

    RefreshStatus();
    RefreshMovement();
}

std::optional<std::string> HudRenderer::Update(float guiScale, const ChatBox& chat)
{
    if (!ready || swapchainExtent.width == 0 || swapchainExtent.height == 0) return std::nullopt;

    int newScale = std::max(1, static_cast<int>(std::lround(guiScale)));
    bool resized = swapchainExtent.width != width || swapchainExtent.height != height;
    width = swapchainExtent.width;
    height = swapchainExtent.height;
    canvas->SetSize(width, height);

    float maxHealth = static_cast<float>(state->attributes->GetDouble("generic.max_health", 20.0));
    int heartCount = std::max(1, static_cast<int>(std::ceil(maxHealth * 0.5f)));
    if (newScale != scale || resized || heartCount != builtHearts)
    {
        scale = newScale;
        Build();
    }

    Refresh();
    RefreshChat(chat);
    input.Feed(*canvas, static_cast<float>(scale));
    Pincel::Texture* texture = canvas->Draw();

    if (chat.open)
    {
        if (backspaceWatch && chatInput.GetText() == backspaceText) SetCommandMode(false);
        backspaceWatch = false;

        // A "/" typed or pasted at the start becomes the badge.
        std::string typed = chatInput.GetText();
        if (!commandMode && !typed.empty() && typed[0] == '/')
        {
            SetCommandMode(true);
            chatInput.SetText(typed.substr(1));
        }
    }

    // Enter sends the set-aside lines and this one; Shift+Enter sets this one aside.
    std::optional<std::string> submitted;
    bool sending = false;
    if (chat.open && chatInput.InputSubmitted())
    {
        if (ImGui::GetIO().KeyShift)
        {
            if (!chatInput.GetText().empty())
            {
                chatQueue.push_back(ComposeChat());
                chatInput.SetText("");
                queueDirty = true;
            }
        }
        else
        {
            sending = true;
            for (const std::string& line : chatQueue) outgoing.push_back(line);
            if (!chatInput.GetText().empty()) outgoing.push_back(ComposeChat());
            for (const std::string& line : outgoing) RememberChat(line);
            chatQueue.clear();
            queueDirty = true;
            if (outgoing.empty()) submitted = std::string(); // Nothing typed: just close the box.
        }
    }

    // Lines go out one per interval, the first at once.
    double now = ImGui::GetTime();
    if (!submitted && !outgoing.empty() && (sending || !chat.open) && now >= nextSendTime)
    {
        submitted = std::move(outgoing.front());
        outgoing.pop_front();
        nextSendTime = now + SEND_INTERVAL;
    }
    surface.Upload(*texture);
    return submitted;
}

void HudRenderer::Record(VkCommandBuffer commandBuffer)
{
    if (ready) surface.Record(commandBuffer);
}

} // namespace Volcano::GUI
