#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include "LoadingScreen.hpp"
#include "../ControlRenderer.hpp"
#include "../GUIController.hpp"
#include "../Theme.hpp"

namespace Volcano::GUI {

namespace {

constexpr float LOG_WIDTH = 600.0f;
constexpr float LOG_HEIGHT = 300.0f;
constexpr float LOG_MARGIN = 32.0f;
constexpr float LOG_PADDING = 6.0f;
constexpr float BUTTON_WIDTH = 240.0f;
constexpr size_t MAX_DISPLAYED_LINES = 150; // Only the tail: the backlog is capped much higher, and this redraws as it grows.
constexpr std::chrono::seconds CANCEL_AFTER{ 15 };

// What the screen shows, and what it needs to tell what changed.
struct LoadingState {
    ConnectionPhase phase = ConnectionPhase::Idle;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    size_t shownLines = 0;
    std::string lastLine;

    Pincel::Element status;
    Pincel::Element detail;
    Pincel::Element log;
    Pincel::Element cancel;
};

std::string StatusFor(ConnectionPhase phase, const std::string& detail)
{
    switch (phase)
    {
        case ConnectionPhase::StartingServer: return "Starting server...";
        case ConnectionPhase::Connecting: return detail.empty() ? "Connecting..." : "Connecting to " + detail + "...";
        case ConnectionPhase::Configuring: return "Configuring...";
        case ConnectionPhase::LoadingTerrain: return "Loading terrain...";
        case ConnectionPhase::Disconnecting: return "Disconnecting...";
        case ConnectionPhase::Idle:
        case ConnectionPhase::Playing: break;
    }
    return "";
}

void BuildLoading(ControlRenderer& ui, LoadingState& loading)
{
    using Pincel::Orientation;
    auto units = [&ui](float value) { return static_cast<uint16_t>(ui.Units(value)); };
    Pincel::Element column = ui.Column();

    loading.status = column.AddText("");
    loading.detail = column.AddText("");
    loading.detail.SetForeground(Theme::TEXT_DIM);

    int logWidth = std::min(ui.Units(LOG_WIDTH), ui.Width() - ui.Units(LOG_MARGIN));
    loading.log = column.AddStack(Orientation::Vertical);
    loading.log.SetSize(static_cast<uint16_t>(std::max(1, logWidth)), units(LOG_HEIGHT))
        .SetPadding(units(LOG_PADDING))
        .SetBackground(Theme::PANEL_DARK)
        .SetBorder(units(2.0f), Theme::PANEL_LIGHT)
        .SetCornerRadius(units(8.0f))
        .SetOverflow(Pincel::Overflow::Scroll)
        .SetStickToEnd(true) // Follows new lines unless it was scrolled up.
        .SetVisible(false);
    loading.shownLines = 0;
    loading.lastLine.clear();

    loading.cancel = ui.AddButton(column, "Cancel", BUTTON_WIDTH);
    loading.cancel.SetVisible(false);
}

void UpdateLoading(ControlRenderer& ui, GlobalState* state, LoadingState& loading, const std::function<void()>& onCancel)
{
    ConnectionPhase phase = state->connectionPhase.load();
    if (phase != loading.phase)
    {
        loading.phase = phase;
        loading.since = std::chrono::steady_clock::now();
    }

    std::string detail = state->GetConnectionDetail();
    loading.status.SetText(StatusFor(phase, detail));
    bool starting = phase == ConnectionPhase::StartingServer;
    bool configuring = phase == ConnectionPhase::Configuring && !detail.empty();
    loading.detail.SetText(configuring ? detail : "");
    loading.detail.SetVisible(configuring);

    loading.log.SetVisible(starting);
    if (starting)
    {
        std::lock_guard<std::mutex> lock(state->serverLogMutex);
        const std::deque<std::string>& lines = state->serverLogLines;
        const std::string& last = lines.empty() ? loading.lastLine : lines.back();
        if (lines.size() != loading.shownLines || last != loading.lastLine)
        {
            loading.log.RemoveChildren();
            size_t start = lines.size() > MAX_DISPLAYED_LINES ? lines.size() - MAX_DISPLAYED_LINES : 0;
            for (size_t i = start; i < lines.size(); i++) loading.log.AddText(lines[i]);
            loading.shownLines = lines.size();
            loading.lastLine = last;
        }
    }

    // Cancelling is only offered after a while, since there is no automatic timeout.
    bool canCancel = starting && std::chrono::steady_clock::now() - loading.since >= CANCEL_AFTER;
    loading.cancel.SetVisible(canCancel);
    if (canCancel && loading.cancel.WasClicked())
    {
        ui.PlayClick();
        if (onCancel) onCancel();
    }
}

} // namespace

Screen* CreateLoadingScreen(GlobalState* state, std::function<void()> onCancel)
{
    Screen* screen = GUIController::CreateScreen("Joining World", /* closable */ false);
    screen->showTitle = false;
    screen->fontSize = Theme::SMALL_FONT_SIZE;
    screen->fontInArtPixels = true;

    auto loading = std::make_shared<LoadingState>();
    screen->build = [loading](ControlRenderer& ui) { BuildLoading(ui, *loading); };
    screen->update = [state, loading, onCancel = std::move(onCancel)](ControlRenderer& ui)
    {
        UpdateLoading(ui, state, *loading, onCancel);
    };
    return screen;
}

} // namespace Volcano::GUI
