#pragma once
#ifndef VOLCANO_GUI_HUD_RENDERER_H
#define VOLCANO_GUI_HUD_RENDERER_H

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <Pincel/Pincel.hpp>
#include "Animation.hpp"
#include "CommandHelp.hpp"
#include "CommandStyle.hpp"
#include "CommandWalker.hpp"
#include "ImGuiInput.hpp"
#include "ItemSlotView.hpp"
#include "PincelSurface.hpp"

namespace Volcano {
struct GlobalState;
}

namespace Volcano::GUI {

// The in-game HUD drawn with Pincel: the hotbar and armor slots, health, hunger,
// armor and experience, the movement state, the chat log and its input box.
// It has a canvas of its own, drawn over the frame while no Screen is open.
// Everything here is placed from the hotbar's position, which matches
// GUIController::GetHotbarOrigin(), so mods anchored to that stay lined up.
// Render thread only.
class HudRenderer {
public:
    explicit HudRenderer(GlobalState* globalState) : state(globalState) {}

    bool Init(VkRenderPass renderPass);
    void Shutdown();

    struct ChatBox {
        bool open = false;
        bool justOpened = false;         // True on the frame it opened, which pre-fills and focuses it.
        std::string initialText;
    };

    // Brings the HUD up to date and draws it to the GPU texture. Returns what was typed
    // if Enter was pressed in the chat box. After GUIController::NewFrame, outside the render pass.
    std::optional<std::string> Update(float guiScale, const ChatBox& chat);

    // Lays the HUD over the frame, inside the main render pass.
    void Record(VkCommandBuffer commandBuffer);

    // Rebuilds from the art again (after a resource reload).
    void ReloadResources();

private:
    static constexpr int HOTBAR_SLOTS = 9;
    static constexpr int ARMOR_SLOTS = 4;
    static constexpr int ICONS_PER_ROW = 10;

    struct HeartIcon { Pincel::Element full; Pincel::Element half; };
    struct FoodIcon { Pincel::Element full; Pincel::Element half; Pincel::Element saturation; Pincel::Element halfSaturation; };
    struct ArmorIcon { Pincel::Element full; Pincel::Element half; };

    void Build();
    void BuildStatus(Pincel::Element parent, int art);
    void Refresh();
    void RefreshStatus();
    void RefreshMovement();
    void RefreshChat(const ChatBox& chat);
    void RefreshChatNavigation();
    void RememberChat(const std::string& text);
    void SetCommandMode(bool on);
    void LoadChat(const std::string& text);
    void RefreshChatPopup(bool open);
    void RebuildChatPopup();
    void PlaceChatPopup(bool open);
    void AnalyzeChat(const std::string& text);
    void CompleteChat(bool backwards);
    std::string ComposeChat() const;
    int Art(float pixels) const;
    int FontPixels(float pixels) const;
    int Units(float units) const;

    Pincel::Element Place(int left, int top, int width, int height);
    // A HUD sprite of `logicalWidth` x `logicalHeight` vanilla texture pixels, whatever its real resolution.
    Pincel::Element AddSprite(Pincel::Element parent, const std::string& path, int logicalWidth, int logicalHeight,
        int left, int top, int art, int cropWidth = 0);
    Pincel::Element Corner(Pincel::Element element, int left, int top);

    GlobalState* state;
    PincelSurface surface;
    std::unique_ptr<Pincel::Canvas> canvas;
    ImGuiInput input;
    bool ready = false;

    int scale = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int builtHearts = 0;
    uint64_t chatRevision = ~0ull;
    std::vector<std::string> chatHistory;   // What was sent from the box, oldest first.
    size_t historyPosition = 0;             // Into chatHistory; its size means the draft.
    std::string historyDraft;
    bool commandMode = false;               // The box holds a command; the "/" is the badge, not text.
    bool backspaceWatch = false;            // Backspace was pressed this frame in command mode.
    std::string backspaceText;
    std::vector<std::string> chatQueue;     // Lines set aside with Shift+Enter, as they will be sent.
    std::deque<std::string> outgoing;       // Lines accepted but not yet handed out, one per send interval.
    double nextSendTime = 0.0;
    bool queueDirty = true;                 // The queue lines or the popup need rebuilding.
    CommandStyle commandStyle;
    CommandHelp commandHelp;
    bool popupHasContent = false;
    std::string commandDescription;         // What the command being typed does, from commands.json.

    // Command mode's reading of the box, redone when the text, the server's tree or the cycling changes.
    CommandAnalysis analysis;
    std::shared_ptr<const CommandTree> analyzedTree;
    std::string analyzedText;
    bool analyzed = false;
    int shownCycle = -1;
    // Tab cycling: what it offers, what came before the word it replaces, and the text it last wrote.
    std::vector<CommandSuggestion> cycleList;
    int cycleIndex = -1;
    std::string cycleBase;
    std::string cycleApplied;

    std::array<ItemSlotView, HOTBAR_SLOTS> hotbarSlots;
    std::array<ItemSlotView, ARMOR_SLOTS> armorSlots;
    Pincel::Element selection;
    std::vector<HeartIcon> hearts;
    std::array<FoodIcon, ICONS_PER_ROW> food;
    std::array<ArmorIcon, ICONS_PER_ROW> armorIcons;
    Pincel::Element armorRow;
    Pincel::Element experienceFill;
    Pincel::Element levelText;
    Pincel::Element movementText;
    Pincel::Element chatLog;
    Pincel::Element chatBox;
    Pincel::Element chatTyping;
    Pincel::Element chatInput;
    Pincel::Element chatBadge;
    Pincel::Element chatColumn;             // The box and the queued lines under it.
    Pincel::Element chatQueueBox;
    Pincel::Element chatPopup;              // The usage tooltip and suggestions, anchored under the word being typed.
    int shownExperienceWidth = -1;
    int shownLevel = -1;

    AnimationSet<float> animations;
};

} // namespace Volcano::GUI

#endif
