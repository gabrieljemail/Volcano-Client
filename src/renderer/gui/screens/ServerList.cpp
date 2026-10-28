#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "ServerList.hpp"
#include "../ControlRenderer.hpp"
#include "../GUIController.hpp"
#include "../../../Logger.hpp"

namespace Volcano::GUI {

namespace {

using nlohmann::json;

std::filesystem::path ServerListPath()
{
    return std::filesystem::current_path() / SERVER_LIST_FILE;
}

std::string Trim(const std::string& text)
{
    size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// The screen's data, shared by its callbacks, and the elements of the current build that update reacts to.
struct ListState {
    struct Row {
        Pincel::Element join;
        Pincel::Element remove;
    };

    std::vector<ServerEntry> servers;
    std::string name;
    std::string address;
    std::string error;

    std::vector<Row> rows;
    Pincel::Element nameInput;
    Pincel::Element addressInput;
    Pincel::Element addButton;
    Pincel::Element directButton;
    Pincel::Element backButton;
    Pincel::Element errorText;
};

constexpr float ROW_WIDTH = 480.0f;
constexpr float ROW_HEIGHT = 48.0f;
constexpr float ROW_GAP = 4.0f;
constexpr float ROW_PADDING = 10.0f;
constexpr float SMALL_BUTTON = 76.0f;
constexpr float NAME_WIDTH = 150.0f;
constexpr float ADD_WIDTH = 80.0f;
constexpr float FIELD_GAP = 6.0f;
constexpr float MENU_BUTTON_WIDTH = 240.0f;
constexpr int VISIBLE_ROWS = 5;

// Appends the form's server to the list and saves. Returns false for an empty address.
bool AddFromForm(ListState& list)
{
    std::string address = Trim(list.address);
    if (address.empty()) return false;

    ServerEntry entry;
    std::string name = Trim(list.name);
    if (!name.empty()) entry.name = name;
    entry.address = address;
    list.servers.push_back(entry);

    list.error = SaveServers(list.servers) ? "" : "Could not save servers.json";
    list.name.clear();
    list.address.clear();
    return true;
}

// The saved servers with Join and Remove, the form to add one, Direct Connect and Back.
void BuildList(ControlRenderer& ui, ListState& list)
{
    using Pincel::Align;
    using Pincel::Color;
    using Pincel::Orientation;

    auto units = [&ui](float value) { return static_cast<uint16_t>(ui.Units(value)); };
    Pincel::Element column = ui.Column();

    int shownRows = std::clamp(static_cast<int>(list.servers.size()), 1, VISIBLE_ROWS);
    Pincel::Element box = column.AddStack(Orientation::Vertical);
    box.SetSize(units(ROW_WIDTH), static_cast<uint16_t>(ui.Units(shownRows * (ROW_HEIGHT + ROW_GAP))))
        .SetGap(units(ROW_GAP))
        .SetOverflow(Pincel::Overflow::Scroll);

    list.rows.clear();
    if (list.servers.empty())
    {
        box.SetAlign(Align::Center, Align::Center);
        box.AddText("No servers saved. Add one below.").SetForeground(Color{ 153, 153, 153 });
    }
    for (const ServerEntry& server : list.servers)
    {
        Pincel::Element row = box.AddStack(Orientation::Horizontal);
        row.SetSize(units(ROW_WIDTH), units(ROW_HEIGHT))
            .SetAlign(Align::Start, Align::Center)
            .SetPadding(0, units(ROW_PADDING), 0, units(ROW_PADDING))
            .SetGap(units(ROW_GAP))
            .SetBackground(Color{ 0, 0, 0, 64.0f / 255.0f });

        // Wide enough that the buttons sit at the right.
        float infoWidth = ROW_WIDTH - 2.0f * ROW_PADDING - 2.0f * SMALL_BUTTON - 2.0f * ROW_GAP;
        Pincel::Element info = row.AddStack(Orientation::Vertical);
        info.SetSize(units(infoWidth), 0);
        info.AddText(server.name);
        info.AddText(server.address).SetForeground(Color{ 160, 160, 160 });

        ListState::Row entry;
        entry.join = ui.AddButton(row, "Join", SMALL_BUTTON);
        entry.remove = ui.AddButton(row, "Remove", SMALL_BUTTON);
        list.rows.push_back(entry);
    }

    column.AddStack(Orientation::Vertical).SetSize(1, units(ROW_GAP));
    Pincel::Element form = column.AddStack(Orientation::Horizontal);
    form.SetGap(units(FIELD_GAP));
    list.nameInput = ui.AddField(form, NAME_WIDTH, "Server name");
    list.addressInput = ui.AddField(form, ROW_WIDTH - NAME_WIDTH - ADD_WIDTH - 2.0f * FIELD_GAP, "Address (host:port)");
    list.addButton = ui.AddButton(form, "Add", ADD_WIDTH);
    list.nameInput.SetText(list.name);
    list.addressInput.SetText(list.address);

    list.errorText = column.AddText(list.error);
    list.errorText.SetForeground(Color{ 255, 102, 102 }).SetVisible(!list.error.empty());

    column.AddStack(Orientation::Vertical).SetSize(1, units(8.0f));
    list.directButton = ui.AddButton(column, "Direct Connect", MENU_BUTTON_WIDTH);
    list.backButton = ui.AddButton(column, "Back", MENU_BUTTON_WIDTH);
}

void UpdateList(ControlRenderer& ui, ListState& list, const ServerListActions& actions)
{
    if (list.nameInput.InputChanged()) list.name = list.nameInput.GetText();
    if (list.addressInput.InputChanged()) list.address = list.addressInput.GetText();

    if (list.addButton.WasClicked() || list.addressInput.InputSubmitted())
    {
        if (list.addButton.WasClicked()) ui.PlayClick();
        if (AddFromForm(list)) ui.Rebuild();
        return;
    }

    for (size_t i = 0; i < list.rows.size(); i++)
    {
        if (list.rows[i].join.WasClicked())
        {
            ui.PlayClick();
            ServerEntry server = list.servers[i];
            if (actions.join) actions.join(server);
            return;
        }
        if (list.rows[i].remove.WasClicked())
        {
            ui.PlayClick();
            list.servers.erase(list.servers.begin() + static_cast<std::ptrdiff_t>(i));
            list.error = SaveServers(list.servers) ? "" : "Could not save servers.json";
            ui.Rebuild();
            return;
        }
    }

    if (list.directButton.WasClicked())
    {
        ui.PlayClick();
        if (actions.directConnect) actions.directConnect();
    }
    else if (list.backButton.WasClicked())
    {
        ui.PlayClick();
        if (actions.back) actions.back();
    }
}

} // namespace

std::vector<ServerEntry> GetServers()
{
    namespace fs = std::filesystem;
    const fs::path path = ServerListPath();

    std::error_code error;
    if (!fs::exists(path, error))
    {
        std::vector<ServerEntry> empty;
        SaveServers(empty); // Leaves a file to edit by hand.
        return empty;
    }

    std::ifstream stream(path);
    if (!stream.is_open())
    {
        Log::Error("Failed to open the server list file.");
        return {};
    }

    try
    {
        json parsed = json::parse(stream);
        if (!parsed.is_array()) throw std::runtime_error("the server list must be an array");

        std::vector<ServerEntry> list;
        for (const json& entry : parsed)
        {
            ServerEntry server;
            server.name = entry.value("name", server.name);
            server.address = entry.value("address", std::string());
            if (!server.address.empty()) list.push_back(server);
        }
        return list;
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("Server list file is invalid (") + e.what() + "); moved to servers.json.bad.");
        stream.close();
        fs::rename(path, path.string() + ".bad", error);
        return {};
    }
}

bool SaveServers(const std::vector<ServerEntry>& servers)
{
    json out = json::array();
    for (const ServerEntry& server : servers) out.push_back({ { "name", server.name }, { "address", server.address } });

    std::ofstream stream(ServerListPath());
    if (!stream.is_open())
    {
        Log::Error("Failed to write the server list file.");
        return false;
    }
    stream << out.dump(4) << "\n";
    return stream.good();
}

Screen* CreateServerList(ServerListActions actions)
{
    Screen* screen = GUIController::CreateScreen("Multiplayer", /* closable */ false);

    auto list = std::make_shared<ListState>();
    list->servers = GetServers();

    screen->build = [list](ControlRenderer& ui) { BuildList(ui, *list); };
    screen->update = [list, actions](ControlRenderer& ui) { UpdateList(ui, *list, actions); };
    return screen;
}

} // namespace Volcano::GUI
