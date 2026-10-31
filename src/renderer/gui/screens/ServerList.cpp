#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <nlohmann/json.hpp>
#include "ServerList.hpp"
#include "../ControlRenderer.hpp"
#include "../GUIController.hpp"
#include "../../../Logger.hpp"
#include "../../../network/ServerPing.hpp"

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
        Pincel::Element card;
        Pincel::Element icon;
        Pincel::Element check;    // The checkbox overlaid on the icon.
        Pincel::Element checkBox;
        Pincel::Element name;
        Pincel::Element motd;
        Pincel::Element pingIcon; // Signal bars, colored by latency.
        Pincel::Element pingText; // Players online and latency.
        Pincel::Element join;
        Pincel::Element edit;
        Pincel::Element remove;
        bool hasIcon = false;     // Whether `icon` was created.
        bool iconShown = false;   // Whether it shows the server's own icon rather than the placeholder.
        int pingTierShown = -1;   // Which signal icon is showing.
        std::string motdShown;    // What the text elements show now, so a frame only sets them on a change.
        std::string pingShown;
    };

    // Pings run on worker threads and land here, keyed by address. The update callback reads them each frame.
    struct Pings {
        std::mutex mutex;
        std::map<std::string, ServerStatus> results;
        std::set<std::string> started;
    };
    std::shared_ptr<Pings> pings = std::make_shared<Pings>();

    std::vector<ServerEntry> servers;
    std::string error;

    std::vector<Row> rows;       // In display order: grouped by category.
    std::vector<size_t> order;   // rows[i] is servers[order[i]].
    std::vector<char> selected;  // One per server: whether its checkbox is ticked.
    Pincel::Element addButton;
    Pincel::Element directButton;
    Pincel::Element backButton;
    Pincel::Element errorText;

    Pincel::Element bar;         // The command bar, shown with two or more selected.
    Pincel::Element barText;
    Pincel::Element moveUpButton;
    Pincel::Element moveDownButton;
    Pincel::Element barRemoveButton;

    Pincel::Element tip;         // The ping tooltip, over everything.
    Pincel::Element tipLines[3];
    std::string tipShown;

    // The add/edit form, shown in place of the list while `editing` is not NOT_EDITING.
    static constexpr int NOT_EDITING = -1;
    static constexpr int ADDING = -2;
    int editing = NOT_EDITING; // Or the index of the server being edited.
    std::string editName;
    std::string editAddress;
    std::string editCategory;
    Pincel::Element editNameInput;
    Pincel::Element editAddressInput;
    Pincel::Element editCategoryInput;
    Pincel::Element saveButton;
    Pincel::Element cancelButton;
};

constexpr float ROW_WIDTH_FRACTION = 0.75f; // Of the window.
constexpr float FORM_WIDTH = 480.0f;
constexpr float ROW_GAP = 4.0f;
constexpr float ROW_PADDING = 10.0f;
constexpr float SMALL_BUTTON = 76.0f;
constexpr float REMOVE_BUTTON = 108.0f;
constexpr float RIGHT_WIDTH = 2.0f * SMALL_BUTTON + REMOVE_BUTTON + 2.0f * ROW_GAP;
constexpr float ICON_SIZE = 40.0f;
constexpr float ICON_PADDING = 4.0f;
constexpr float PING_ICON_SIZE = 20.0f;
constexpr float FIELD_GAP = 6.0f;
constexpr float MENU_BUTTON_WIDTH = 240.0f;
constexpr float CARD_RADIUS = 8.0f;
constexpr float ICON_RADIUS = 6.0f;
constexpr float CHECK_SIZE = 18.0f;
constexpr float BAR_HEIGHT = 56.0f;
constexpr float BAR_BUTTON = 110.0f;
constexpr float CARD_HEIGHT_ESTIMATE = 76.0f;
constexpr float LABEL_HEIGHT_ESTIMATE = 28.0f;
constexpr float CHROME_HEIGHT = 300.0f; // Title and buttons around the list, and room for the command bar.

// Pings the address in the background unless it already has been this session.
void StartPing(ListState& list, const std::string& address)
{
    auto pings = list.pings;
    {
        std::lock_guard<std::mutex> lock(pings->mutex);
        if (!pings->started.insert(address).second) return;
    }
    std::thread([pings, address]()
    {
        ServerStatus status = PingServer(address);
        std::lock_guard<std::mutex> lock(pings->mutex);
        pings->results[address] = status;
    }).detach();
}

// The pinged status for an address, or nothing while it is still in flight.
std::optional<ServerStatus> GetPing(ListState& list, const std::string& address)
{
    std::lock_guard<std::mutex> lock(list.pings->mutex);
    auto found = list.pings->results.find(address);
    if (found == list.pings->results.end()) return std::nullopt;
    return found->second;
}

std::string MotdText(const std::optional<ServerStatus>& status)
{
    if (!status) return "Pinging...";
    if (!status->online) return "Can't connect to server";
    return status->motd.empty() ? status->version : status->motd;
}

std::string PingText(const std::optional<ServerStatus>& status)
{
    if (!status || !status->online) return "";
    return std::to_string(status->playersOnline) + "/" + std::to_string(status->playersMax) + "  " + std::to_string(status->latencyMs) + "ms";
}

// 0 for no answer (yet), then 1 (slow) to 3 (fast).
int PingTier(const std::optional<ServerStatus>& status)
{
    if (!status || !status->online) return 0;
    if (status->latencyMs < 150) return 3;
    if (status->latencyMs < 300) return 2;
    return 1;
}

// A 16x16 signal icon: four bars, as many lit (green, yellow or red) as the tier says.
std::vector<uint8_t> PingIconPixels(int tier)
{
    constexpr int SIZE = 16;
    std::vector<uint8_t> pixels(SIZE * SIZE * 4, 0);
    const int lit = tier == 3 ? 4 : tier;
    const uint8_t color[3] = { tier == 3 ? uint8_t(80) : uint8_t(240), tier == 1 ? uint8_t(70) : uint8_t(tier == 2 ? 200 : 220), tier == 3 ? uint8_t(80) : uint8_t(60) };
    for (int bar = 0; bar < 4; bar++)
    {
        const int height = 4 * (bar + 1);
        for (int y = SIZE - height; y < SIZE; y++)
        {
            for (int x = bar * 4; x < bar * 4 + 3; x++)
            {
                uint8_t* pixel = &pixels[static_cast<size_t>(y * SIZE + x) * 4];
                if (bar < lit)
                {
                    pixel[0] = color[0]; pixel[1] = color[1]; pixel[2] = color[2];
                }
                else
                {
                    pixel[0] = pixel[1] = pixel[2] = 90;
                }
                pixel[3] = 255;
            }
        }
    }
    return pixels;
}

std::string CategoryKey(const std::string& category)
{
    std::string key = category;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

// Server indices in display order: uncategorized first, then categories alphabetically, each in saved order.
std::vector<size_t> DisplayOrder(const std::vector<ServerEntry>& servers)
{
    std::vector<size_t> order(servers.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&servers](size_t a, size_t b)
    {
        return CategoryKey(servers[a].category) < CategoryKey(servers[b].category);
    });
    return order;
}

// Shows the add/edit form in place of the list.
void BuildEdit(ControlRenderer& ui, ListState& list)
{
    using Pincel::Color;

    auto units = [&ui](float value) { return static_cast<uint16_t>(ui.Units(value)); };
    Pincel::Element column = ui.Column();
    list.rows.clear();

    column.AddText(list.editing == ListState::ADDING ? "Add Server" : "Edit Server");
    auto addLabeled = [&](const char* label, const std::string& value, const char* hint)
    {
        column.AddText(label).SetForeground(Color{ 160, 160, 160 });
        Pincel::Element input = ui.AddField(column, FORM_WIDTH, hint);
        input.SetText(value);
        return input;
    };
    list.editNameInput = addLabeled("Server name", list.editName, "Minecraft server");
    list.editAddressInput = addLabeled("Address", list.editAddress, "host:port");
    list.editCategoryInput = addLabeled("Category", list.editCategory, "None");

    column.AddStack(Pincel::Orientation::Vertical).SetSize(1, units(8.0f));
    Pincel::Element buttons = column.AddStack(Pincel::Orientation::Horizontal);
    buttons.SetGap(units(FIELD_GAP));
    list.saveButton = ui.AddButton(buttons, list.editing == ListState::ADDING ? "Add" : "Save", MENU_BUTTON_WIDTH / 2.0f);
    list.cancelButton = ui.AddButton(buttons, "Cancel", MENU_BUTTON_WIDTH / 2.0f);

    list.errorText = column.AddText(list.error);
    list.errorText.SetForeground(Color{ 255, 102, 102 }).SetVisible(!list.error.empty());
}

// The saved servers grouped by category (one card each, selectable), Add Server, Direct Connect and Back,
// and a command bar at the bottom for acting on several selected servers at once.
void BuildList(ControlRenderer& ui, ListState& list)
{
    using Pincel::Align;
    using Pincel::Color;
    using Pincel::Orientation;

    if (list.editing != ListState::NOT_EDITING)
    {
        BuildEdit(ui, list);
        return;
    }

    auto units = [&ui](float value) { return static_cast<uint16_t>(ui.Units(value)); };
    Pincel::Element column = ui.Column();

    list.selected.resize(list.servers.size(), 0);
    list.order = DisplayOrder(list.servers);

    // Cards are three quarters of the window wide.
    const float rowWidth = std::max(ROW_WIDTH_FRACTION * static_cast<float>(ui.Width()) / static_cast<float>(ui.Units(1.0f)), 2.0f * RIGHT_WIDTH);
    const float iconBoxSize = ICON_SIZE + 2.0f * ICON_PADDING;

    // The scroll area spans the screen and is as tall as its cards need, up to what the screen has room for.
    size_t labelCount = 0;
    for (size_t k = 0; k < list.order.size(); k++)
    {
        const std::string& category = list.servers[list.order[k]].category;
        if (!category.empty() && (k == 0 || CategoryKey(category) != CategoryKey(list.servers[list.order[k - 1]].category))) labelCount++;
    }
    const float wanted = std::max<float>(1.0f, static_cast<float>(list.servers.size())) * (CARD_HEIGHT_ESTIMATE + ROW_GAP)
        + static_cast<float>(labelCount) * LABEL_HEIGHT_ESTIMATE;
    const int boxHeight = std::min(ui.Units(wanted), std::max(ui.Units(2.0f * CARD_HEIGHT_ESTIMATE), ui.Height() - ui.Units(CHROME_HEIGHT)));
    Pincel::Element box = column.AddStack(Orientation::Vertical);
    box.SetSize(static_cast<uint16_t>(ui.Width()), static_cast<uint16_t>(boxHeight))
        .SetAlign(Align::Center, Align::Start)
        .SetGap(units(ROW_GAP))
        .SetOverflow(Pincel::Overflow::Scroll);

    list.rows.clear();
    if (list.servers.empty())
    {
        box.SetAlign(Align::Center, Align::Center);
        box.AddText("No servers saved. Add one below.").SetForeground(Color{ 153, 153, 153 });
    }
    for (size_t k = 0; k < list.order.size(); k++)
    {
        const ServerEntry& server = list.servers[list.order[k]];

        // A label whenever the category changes.
        if (!server.category.empty() && (k == 0 || CategoryKey(server.category) != CategoryKey(list.servers[list.order[k - 1]].category)))
        {
            Pincel::Element label = box.AddStack(Orientation::Horizontal);
            label.SetSize(units(rowWidth), 0).SetAlign(Align::Start, Align::Center).SetPadding(units(ROW_GAP), units(ROW_PADDING), 0, units(ROW_PADDING));
            label.AddText(server.category).SetForeground(Color{ 255, 170, 0 });
        }

        ListState::Row entry;
        entry.card = box.AddStack(Orientation::Horizontal);
        entry.card.SetSize(units(rowWidth), 0)
            .SetAlign(Align::Start, Align::Start)
            .SetPadding(units(ROW_PADDING))
            .SetGap(units(ROW_GAP))
            .SetBackground(Color{ 16, 16, 16, 0.85f })
            .SetBorder(units(1.0f), Color{ 0, 0, 0, 0.0f })
            .SetCornerRadius(units(CARD_RADIUS));

        StartPing(list, server.address);
        std::optional<ServerStatus> status = GetPing(list, server.address);

        // The server's icon, or a grey square until it arrives (or if it sends none), with a little padding.
        Pincel::Element iconBox = entry.card.AddStack(Orientation::Vertical);
        iconBox.SetSize(units(iconBoxSize), units(iconBoxSize)).SetPadding(units(ICON_PADDING));
        const bool hasIcon = status && !status->icon.empty();
        std::vector<uint8_t> blank(64 * 64 * 4, 60);
        std::optional<Pincel::Element> icon = hasIcon
            ? iconBox.AddImage(status->icon.data(), status->iconWidth, status->iconHeight)
            : iconBox.AddImage(blank.data(), 64, 64);
        if (icon)
        {
            entry.icon = *icon;
            entry.icon.SetSize(units(ICON_SIZE), units(ICON_SIZE))
                .SetImageFilter(Pincel::ImageFilter::Nearest)
                .SetCornerRadius(units(ICON_RADIUS));
            entry.iconShown = hasIcon;
            entry.hasIcon = true;
        }

        // Laid over the icon: a checkbox button, shown while the card is hovered or anything is selected.
        entry.check = iconBox.AddStack(Orientation::Vertical);
        entry.check.SetPosition(Pincel::Position::Absolute)
            .SetInsets(Pincel::Insets{ .left = 0, .top = 0, .right = 0, .bottom = 0 })
            .SetAlign(Align::Center, Align::Center)
            .SetBackground(Color{ 0, 0, 0, 0.55f })
            .SetCornerRadius(units(ICON_RADIUS))
            .SetVisible(false);
        entry.checkBox = entry.check.AddStack(Orientation::Vertical);
        entry.checkBox.SetSize(units(CHECK_SIZE), units(CHECK_SIZE))
            .SetBackground(Color{ 0, 0, 0, 0.6f })
            .SetBorder(units(2.0f), Color{ 255, 255, 255 })
            .SetCornerRadius(units(3.0f));

        Pincel::Element info = entry.card.AddStack(Orientation::Vertical);
        info.SetSize(units(rowWidth - 2.0f * ROW_PADDING - iconBoxSize - RIGHT_WIDTH - 2.0f * ROW_GAP), 0);
        entry.name = info.AddText(server.name);
        entry.motdShown = MotdText(status);
        entry.motd = info.AddText(entry.motdShown);
        entry.motd.SetForeground(Color{ 160, 160, 160 });

        // Buttons in the top half of the right side, ping stats in the bottom half.
        Pincel::Element right = entry.card.AddStack(Orientation::Vertical);
        right.SetSize(units(RIGHT_WIDTH), 0).SetAlign(Align::End, Align::Start).SetGap(units(ROW_GAP));

        Pincel::Element buttons = right.AddStack(Orientation::Horizontal);
        buttons.SetAlign(Align::End, Align::Center).SetGap(units(ROW_GAP));
        entry.join = ui.AddButton(buttons, "Join", SMALL_BUTTON);
        entry.edit = ui.AddButton(buttons, "Edit", SMALL_BUTTON);
        entry.remove = ui.AddButton(buttons, "Remove", REMOVE_BUTTON);

        Pincel::Element stats = right.AddStack(Orientation::Horizontal);
        stats.SetAlign(Align::End, Align::Center).SetGap(units(ROW_GAP));
        entry.pingShown = PingText(status);
        entry.pingText = stats.AddText(entry.pingShown);
        entry.pingText.SetForeground(Color{ 160, 160, 160 });
        entry.pingTierShown = PingTier(status);
        std::vector<uint8_t> signal = PingIconPixels(entry.pingTierShown);
        if (std::optional<Pincel::Element> pingIcon = stats.AddImage(signal.data(), 16, 16))
        {
            entry.pingIcon = *pingIcon;
            entry.pingIcon.SetSize(units(PING_ICON_SIZE), units(PING_ICON_SIZE)).SetImageFilter(Pincel::ImageFilter::Nearest);
        }
        list.rows.push_back(entry);
    }

    column.AddStack(Orientation::Vertical).SetSize(1, units(ROW_GAP));
    list.errorText = column.AddText(list.error);
    list.errorText.SetForeground(Color{ 255, 102, 102 }).SetVisible(!list.error.empty());

    column.AddStack(Orientation::Vertical).SetSize(1, units(8.0f));
    list.addButton = ui.AddButton(column, "Add Server", MENU_BUTTON_WIDTH);
    list.directButton = ui.AddButton(column, "Direct Connect", MENU_BUTTON_WIDTH);
    list.backButton = ui.AddButton(column, "Back", MENU_BUTTON_WIDTH);

    // The command bar, over the bottom edge. Only shown with two or more servers selected.
    list.bar = ui.Overlay().AddStack(Orientation::Horizontal);
    list.bar.SetPosition(Pincel::Position::Fixed)
        .SetInsets(Pincel::Insets{ .left = 0, .right = 0, .bottom = 0 })
        .SetSize(0, units(BAR_HEIGHT))
        .SetAlign(Align::Center, Align::Center)
        .SetGap(units(FIELD_GAP))
        .SetBackground(Color{ 16, 16, 16, 0.85f })
        .SetVisible(false);
    list.barText = list.bar.AddText("");
    list.moveUpButton = ui.AddButton(list.bar, "Move Up", BAR_BUTTON);
    list.moveDownButton = ui.AddButton(list.bar, "Move Down", BAR_BUTTON);
    list.barRemoveButton = ui.AddButton(list.bar, "Remove", BAR_BUTTON);

    // The ping tooltip follows the mouse over a signal icon.
    list.tip = ui.Overlay().AddStack(Orientation::Vertical);
    list.tip.SetPosition(Pincel::Position::Fixed)
        .SetPointerEvents(Pincel::PointerEvents::None)
        .SetPadding(units(ROW_GAP + 2.0f))
        .SetBackground(Color{ 16, 16, 16, 0.95f })
        .SetBorder(units(1.0f), Color{ 90, 90, 90 })
        .SetCornerRadius(units(4.0f))
        .SetVisible(false);
    for (Pincel::Element& line : list.tipLines) line = list.tip.AddText("");
    list.tipShown.clear();
}

int SelectedCount(const ListState& list)
{
    return static_cast<int>(std::count(list.selected.begin(), list.selected.end(), 1));
}

// Moves the selected servers one place up or down within their category, keeping a block of neighbours together.
// The list is saved in display order afterwards.
void MoveSelected(ListState& list, bool up)
{
    std::vector<size_t> order = DisplayOrder(list.servers);
    const size_t count = order.size();
    for (size_t step = 0; step < count; step++)
    {
        const size_t i = up ? step : count - 1 - step;
        if (up ? i == 0 : i + 1 == count) continue;
        const size_t other = up ? i - 1 : i + 1;
        if (!list.selected[order[i]] || list.selected[order[other]]) continue;
        if (CategoryKey(list.servers[order[i]].category) != CategoryKey(list.servers[order[other]].category)) continue;
        std::swap(order[i], order[other]);
    }

    std::vector<ServerEntry> servers;
    std::vector<char> selected;
    for (size_t index : order)
    {
        servers.push_back(list.servers[index]);
        selected.push_back(list.selected[index]);
    }
    list.servers = std::move(servers);
    list.selected = std::move(selected);
}

void RemoveSelected(ListState& list)
{
    for (size_t i = list.servers.size(); i-- > 0;)
    {
        if (!list.selected[i]) continue;
        list.servers.erase(list.servers.begin() + static_cast<std::ptrdiff_t>(i));
        list.selected.erase(list.selected.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void UpdateEdit(ControlRenderer& ui, ListState& list)
{
    if (list.editNameInput.InputChanged()) list.editName = list.editNameInput.GetText();
    if (list.editAddressInput.InputChanged()) list.editAddress = list.editAddressInput.GetText();
    if (list.editCategoryInput.InputChanged()) list.editCategory = list.editCategoryInput.GetText();

    if (list.saveButton.WasClicked() || list.editAddressInput.InputSubmitted())
    {
        ui.PlayClick();
        std::string address = Trim(list.editAddress);
        if (address.empty())
        {
            list.error = "The address can't be empty";
        }
        else
        {
            if (list.editing == ListState::ADDING)
            {
                list.servers.emplace_back();
                list.selected.push_back(0);
            }
            ServerEntry& server = list.servers[list.editing == ListState::ADDING ? list.servers.size() - 1 : static_cast<size_t>(list.editing)];
            std::string name = Trim(list.editName);
            server.name = name.empty() ? "Minecraft server" : name;
            server.address = address;
            server.category = Trim(list.editCategory);
            list.error = SaveServers(list.servers) ? "" : "Could not save servers.json";
            list.editing = ListState::NOT_EDITING;
        }
        ui.Rebuild();
    }
    else if (list.cancelButton.WasClicked())
    {
        ui.PlayClick();
        list.editing = ListState::NOT_EDITING;
        list.error.clear();
        ui.Rebuild();
    }
}

// What the ping tooltip says for a server's status.
std::array<std::string, 3> PingTipLines(const std::optional<ServerStatus>& status)
{
    if (!status) return { "Pinging...", "", "" };
    if (!status->online) return { "No response", status->error, "" };
    return { "Latency: " + std::to_string(status->latencyMs) + " ms",
             "Players: " + std::to_string(status->playersOnline) + "/" + std::to_string(status->playersMax),
             "Version: " + status->version };
}

void UpdateList(ControlRenderer& ui, ListState& list, const ServerListActions& actions)
{
    using Pincel::Color;

    if (list.editing != ListState::NOT_EDITING)
    {
        UpdateEdit(ui, list);
        return;
    }

    list.selected.resize(list.servers.size(), 0);
    const int selectedCount = SelectedCount(list);
    const size_t rowCount = std::min(list.rows.size(), list.order.size());
    int hoveredPing = -1;

    for (size_t i = 0; i < rowCount; i++)
    {
        ListState::Row& row = list.rows[i];
        const size_t server = list.order[i];
        std::optional<ServerStatus> status = GetPing(list, list.servers[server].address);
        std::string motd = MotdText(status);
        std::string ping = PingText(status);
        if (row.hasIcon && !row.iconShown && status && !status->icon.empty())
            row.iconShown = row.icon.SetImage(status->icon.data(), status->iconWidth, status->iconHeight);
        if (motd != row.motdShown && row.motd.SetText(motd)) row.motdShown = motd;
        if (ping != row.pingShown && row.pingText.SetText(ping)) row.pingShown = ping;
        const int tier = PingTier(status);
        if (tier != row.pingTierShown)
        {
            std::vector<uint8_t> signal = PingIconPixels(tier);
            if (row.pingIcon.SetImage(signal.data(), 16, 16)) row.pingTierShown = tier;
        }

        const bool selected = list.selected[server] != 0;
        row.card.SetBorder(static_cast<uint16_t>(ui.Units(1.0f)), selected ? Color{ 255, 170, 0 } : Color{ 0, 0, 0, 0.0f });
        row.check.SetVisible(selected || selectedCount > 0 || row.card.IsHovered());
        row.checkBox.SetBackground(selected ? Color{ 255, 170, 0 } : Color{ 0, 0, 0, 0.6f });
        if (row.pingIcon.IsHovered()) hoveredPing = static_cast<int>(i);
    }

    // The ping tooltip, beside the mouse.
    if (hoveredPing >= 0 && !std::isnan(ui.Mouse().x))
    {
        const size_t server = list.order[static_cast<size_t>(hoveredPing)];
        std::array<std::string, 3> lines = PingTipLines(GetPing(list, list.servers[server].address));
        const std::string joined = lines[0] + "\n" + lines[1] + "\n" + lines[2];
        if (joined != list.tipShown)
        {
            for (size_t n = 0; n < 3; n++)
            {
                list.tipLines[n].SetText(lines[n]);
                list.tipLines[n].SetVisible(!lines[n].empty());
            }
            list.tipShown = joined;
        }
        list.tip.SetInsets(Pincel::Insets{ .left = static_cast<int>(ui.Mouse().x) + ui.Units(14.0f), .top = static_cast<int>(ui.Mouse().y) + ui.Units(14.0f) });
        list.tip.SetVisible(true);
    }
    else
    {
        list.tip.SetVisible(false);
    }

    list.bar.SetVisible(selectedCount >= 2);
    if (selectedCount >= 2) list.barText.SetText(std::to_string(selectedCount) + " selected");

    for (size_t i = 0; i < rowCount; i++)
    {
        const size_t server = list.order[i];
        if (list.rows[i].check.WasClicked())
        {
            ui.PlayClick();
            list.selected[server] = list.selected[server] ? 0 : 1;
            return;
        }
        if (list.rows[i].join.WasClicked())
        {
            ui.PlayClick();
            ServerEntry entry = list.servers[server];
            if (actions.join) actions.join(entry);
            return;
        }
        if (list.rows[i].edit.WasClicked())
        {
            ui.PlayClick();
            list.editing = static_cast<int>(server);
            list.editName = list.servers[server].name;
            list.editAddress = list.servers[server].address;
            list.editCategory = list.servers[server].category;
            list.error.clear();
            ui.Rebuild();
            return;
        }
        if (list.rows[i].remove.WasClicked())
        {
            ui.PlayClick();
            list.servers.erase(list.servers.begin() + static_cast<std::ptrdiff_t>(server));
            list.selected.erase(list.selected.begin() + static_cast<std::ptrdiff_t>(server));
            list.error = SaveServers(list.servers) ? "" : "Could not save servers.json";
            ui.Rebuild();
            return;
        }
    }

    if (selectedCount >= 2)
    {
        const bool up = list.moveUpButton.WasClicked();
        if (up || list.moveDownButton.WasClicked())
        {
            ui.PlayClick();
            MoveSelected(list, up);
            list.error = SaveServers(list.servers) ? "" : "Could not save servers.json";
            ui.Rebuild();
            return;
        }
        if (list.barRemoveButton.WasClicked())
        {
            ui.PlayClick();
            RemoveSelected(list);
            list.error = SaveServers(list.servers) ? "" : "Could not save servers.json";
            ui.Rebuild();
            return;
        }
    }

    if (list.addButton.WasClicked())
    {
        ui.PlayClick();
        list.editing = ListState::ADDING;
        list.editName.clear();
        list.editAddress.clear();
        list.editCategory.clear();
        list.error.clear();
        ui.Rebuild();
    }
    else if (list.directButton.WasClicked())
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
            server.category = entry.value("category", std::string());
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
    for (const ServerEntry& server : servers) out.push_back({ { "name", server.name }, { "address", server.address }, { "category", server.category } });

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
