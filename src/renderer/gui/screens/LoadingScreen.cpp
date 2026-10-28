#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <imgui.h>
#include "LoadingScreen.hpp"
#include "../GUIController.hpp"

namespace Volcano::GUI {

namespace {

// Screens draw their own centered text here (rather than through the
// generic component stack) because the status line changes every phase.
void CenteredText(const std::string& text, ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f))
{
    float width = ImGui::CalcTextSize(text.c_str()).x;
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
    ImGui::TextColored(color, "%s", text.c_str());
}

struct PhaseTimer {
    ConnectionPhase phase = ConnectionPhase::Idle;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};

} // namespace

Screen* CreateLoadingScreen(GlobalState* state, std::function<void()> onCancel)
{
    Screen* screen = GUIController::CreateScreen("Joining World", /* closable */ false);

    auto timer = std::make_shared<PhaseTimer>();

    screen->customContent = [state, onCancel = std::move(onCancel), timer] {
        ConnectionPhase phase = state->connectionPhase.load();
        if (phase != timer->phase)
        {
            timer->phase = phase;
            timer->since = std::chrono::steady_clock::now();
        }

        std::string detail = state->GetConnectionDetail();
        const ImVec4 dim(0.7f, 0.7f, 0.7f, 1.0f);

        switch (phase)
        {
        case ConnectionPhase::StartingServer:
        {
            CenteredText("Starting server...");
            ImGui::Dummy(ImVec2(0.0f, 8.0f));

            float logWidth = std::min(600.0f, ImGui::GetWindowWidth() - 32.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - logWidth) * 0.5f);
            ImGui::BeginChild("##serverlog", ImVec2(logWidth, 300.0f), true);
            {
                // Only the tail (not all of serverLogLines, capped much
                // higher at 500) — this redraws every frame with no cap of
                // its own on TargetFPS/VSync, so the full backlog would mean
                // hundreds of ImGui::TextUnformatted calls per frame for as
                // long as this screen is up, for something that only ever
                // needs to show recent progress.
                constexpr size_t kMaxDisplayedLines = 150;
                std::lock_guard<std::mutex> lock(state->serverLogMutex);
                size_t start = state->serverLogLines.size() > kMaxDisplayedLines
                    ? state->serverLogLines.size() - kMaxDisplayedLines : 0;
                for (size_t i = start; i < state->serverLogLines.size(); i++)
                {
                    ImGui::TextUnformatted(state->serverLogLines[i].c_str());
                }
            }
            // Only follow the tail if already at (or past) the bottom — a
            // player who's scrolled up to read something shouldn't get
            // yanked back down by the next line arriving.
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();

            ImGui::Dummy(ImVec2(0.0f, 12.0f));

            if (std::chrono::steady_clock::now() - timer->since >= std::chrono::seconds(15))
            {
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 240.0f) * 0.5f);
                if (GUIController::DrawStyledButton("Cancel", ImVec2(240.0f, 0.0f)) && onCancel)
                {
                    onCancel();
                }
            }
            break;
        }
        case ConnectionPhase::Connecting:
            CenteredText(detail.empty() ? "Connecting..." : "Connecting to " + detail + "...");
            break;
        case ConnectionPhase::Configuring:
            CenteredText("Configuring...");
            if (!detail.empty()) CenteredText(detail, dim);
            break;
        case ConnectionPhase::LoadingTerrain:
            CenteredText("Loading terrain...");
            break;
        case ConnectionPhase::Disconnecting:
            CenteredText("Disconnecting...");
            break;
        case ConnectionPhase::Idle:
        case ConnectionPhase::Playing:
            break;
        }
    };

    return screen;
}

} // namespace Volcano::GUI
