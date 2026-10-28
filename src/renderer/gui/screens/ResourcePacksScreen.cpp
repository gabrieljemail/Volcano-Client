#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include "ResourcePacksScreen.hpp"
#include "../GUIController.hpp"
#include "../../../Config.hpp"
#include "../../../GlobalState.hpp"
#include "../../../resources/ResourceReloader.hpp"
#include "../../../resources/ZipArchive.hpp"

namespace Volcano::GUI {

namespace {

namespace fs = std::filesystem;

constexpr float LIST_WIDTH = 300.0f;
constexpr float LIST_HEIGHT = 260.0f;
constexpr float COLUMN_GAP = 16.0f;
constexpr float SMALL_BUTTON_WIDTH = 30.0f;
constexpr float ROW_BUTTON_WIDTH = 64.0f;
constexpr double RESCAN_SECONDS = 2.0;
constexpr const char* BASE_PACK_PATH = "resources/minecraft";

struct PackInfo {
    std::string name;
    std::string description;
};

struct ScreenState {
    std::vector<PackInfo> found; // Packs on disk, by name.
    double lastScan = -1000.0;
    bool changed = false; // Active list edited since the screen was last left.
};

// A pack.mcmeta description as plain text, or empty.
std::string ReadDescription(const std::string& path)
{
    std::optional<std::vector<uint8_t>> bytes;
    if (fs::is_directory(path))
    {
        std::ifstream file(path + "/pack.mcmeta", std::ios::binary);
        if (file) bytes = std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
    }
    else if (auto zip = ZipArchive::Open(path))
    {
        bytes = zip->Read("pack.mcmeta");
    }
    if (!bytes) return {};

    nlohmann::json meta = nlohmann::json::parse(bytes->begin(), bytes->end(), nullptr, false);
    if (!meta.is_object() || !meta.contains("pack") || !meta["pack"].is_object()) return {};
    const nlohmann::json& description = meta["pack"].value("description", nlohmann::json());
    if (description.is_string()) return description.get<std::string>();
    if (description.is_object() && description.contains("text") && description["text"].is_string())
    {
        return description["text"].get<std::string>();
    }
    return {};
}

void Scan(Config& config, std::vector<PackInfo>& out)
{
    out.clear();
    std::set<std::string> seen;
    for (const std::string& dir : { config.ResolvePath("resourcepacks"), std::string("resources/resourcepacks") })
    {
        std::error_code error;
        if (!fs::is_directory(dir, error)) continue;
        for (const fs::directory_entry& entry : fs::directory_iterator(dir, error))
        {
            std::string name = entry.path().filename().string();
            bool isZip = entry.is_regular_file() && entry.path().extension() == ".zip";
            if (!isZip && !entry.is_directory()) continue;
            if (!seen.insert(name).second) continue;
            out.push_back({ name, ReadDescription(entry.path().string()) });
        }
    }
    std::sort(out.begin(), out.end(), [](const PackInfo& a, const PackInfo& b) { return a.name < b.name; });
}

void DrawPacks(GlobalState* state, ScreenState& screen, const std::function<void()>& onDone)
{
    Config& config = *state->config;
    const double now = ImGui::GetTime();
    if (now - screen.lastScan > RESCAN_SECONDS)
    {
        Scan(config, screen.found);
        screen.lastScan = now;
    }

    std::vector<std::string> active = config.GetStringArray("Graphics.ResourcePacks");
    auto describe = [&](const std::string& name) -> const PackInfo*
    {
        for (const PackInfo& info : screen.found)
        {
            if (info.name == name) return &info;
        }
        return nullptr;
    };

    const float totalWidth = LIST_WIDTH * 2.0f + COLUMN_GAP;
    const float left = (ImGui::GetWindowWidth() - totalWidth) * 0.5f;
    bool edited = false;

    // Available: packs on disk that aren't active.
    ImGui::SetCursorPosX(left);
    ImGui::TextDisabled("Available");
    ImGui::SameLine(left + LIST_WIDTH + COLUMN_GAP);
    ImGui::TextDisabled("Active (top wins)");

    ImGui::SetCursorPosX(left);
    ImGui::BeginChild("##available", ImVec2(LIST_WIDTH, LIST_HEIGHT), true);
    int shown = 0;
    for (const PackInfo& info : screen.found)
    {
        if (std::find(active.begin(), active.end(), info.name) != active.end()) continue;
        shown++;
        ImGui::PushID(info.name.c_str());
        GUIController::AlignTextToControl();
        ImGui::TextUnformatted(info.name.c_str());
        ImGui::SameLine(LIST_WIDTH - ROW_BUTTON_WIDTH - ImGui::GetStyle().ScrollbarSize - 12.0f);
        if (GUIController::DrawButton("Add", ROW_BUTTON_WIDTH))
        {
            active.insert(active.begin(), info.name);
            edited = true;
        }
        if (!info.description.empty())
        {
            ImGui::TextDisabled("%s", info.description.c_str());
        }
        ImGui::PopID();
        if (edited) break;
    }
    if (shown == 0) ImGui::TextDisabled("Nothing else in the resourcepacks folder.");
    ImGui::EndChild();

    // Active: in priority order, then the base pack.
    ImGui::SameLine(0.0f, COLUMN_GAP);
    ImGui::BeginChild("##active", ImVec2(LIST_WIDTH, LIST_HEIGHT), true);
    int moveFrom = -1, moveTo = -1, removeAt = -1;
    for (size_t i = 0; i < active.size(); i++)
    {
        const std::string& name = active[i];
        const PackInfo* info = describe(name);
        ImGui::PushID(static_cast<int>(i));
        GUIController::AlignTextToControl();
        if (info == nullptr) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s (missing)", name.c_str());
        else ImGui::TextUnformatted(name.c_str());

        ImGui::SameLine(LIST_WIDTH - SMALL_BUTTON_WIDTH * 2.0f - ROW_BUTTON_WIDTH - 24.0f - ImGui::GetStyle().ScrollbarSize);
        if (GUIController::DrawButton("^", SMALL_BUTTON_WIDTH) && i > 0)
        {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) - 1;
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (GUIController::DrawButton("v", SMALL_BUTTON_WIDTH) && i + 1 < active.size())
        {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) + 1;
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (GUIController::DrawButton("Remove", ROW_BUTTON_WIDTH)) removeAt = static_cast<int>(i);
        if (info != nullptr && !info->description.empty()) ImGui::TextDisabled("%s", info->description.c_str());
        ImGui::PopID();
    }
    if (moveFrom >= 0)
    {
        std::swap(active[static_cast<size_t>(moveFrom)], active[static_cast<size_t>(moveTo)]);
        edited = true;
    }
    if (removeAt >= 0)
    {
        active.erase(active.begin() + removeAt);
        edited = true;
    }

    // The base pack can't be removed or moved: it is the floor every lookup falls to.
    ImGui::Separator();
    bool baseThere = fs::is_directory(BASE_PACK_PATH);
    if (baseThere) ImGui::TextUnformatted("Default (base) - required");
    else ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Default (base) - missing!");
    ImGui::TextDisabled("%s", baseThere ? "The game's own assets. Always the lowest layer." : "Nothing can render without it.");
    ImGui::EndChild();

    if (edited)
    {
        config.SetStringArray("Graphics.ResourcePacks", active);
        screen.changed = true;
    }

    ImGui::SetCursorPosX(left);
    ImGui::TextDisabled("Packs go in the resourcepacks folder next to the game. Changes apply when you press Done.");
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (GUIController::DrawMenuButton("Done"))
    {
        if (screen.changed) ResourceReloader::Request();
        screen.changed = false;
        screen.lastScan = -1000.0;
        if (onDone) onDone();
    }
}

} // namespace

Screen* CreateResourcePacksScreen(GlobalState* state, std::function<void()> onDone)
{
    Screen* screen = GUIController::CreateScreen("Resource Packs", /* closable */ false);

    auto screenState = std::make_shared<ScreenState>();
    screen->customContent = [state, screen, screenState, onDone]
    {
        ImGui::GetFont()->Scale = 1.15f;
        ImVec2 titleSize = ImGui::CalcTextSize(screen->title.c_str());
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleSize.x) * 0.5f);
        ImGui::Text("%s", screen->title.c_str());
        ImGui::GetFont()->Scale = 1.00f;
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        DrawPacks(state, *screenState, onDone);
    };
    return screen;
}

} // namespace Volcano::GUI
