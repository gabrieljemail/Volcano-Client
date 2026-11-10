#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>
#include "WorldManager.hpp"
#include "../ControlRenderer.hpp"
#include "../../../platform/OS.hpp"

namespace Volcano::GUI {

namespace {

constexpr float CARD_WIDTH = 520.0f;
constexpr float CARD_PADDING = 10.0f;
constexpr float CARD_RADIUS = 8.0f;
constexpr float GAP = 4.0f;
constexpr float SMALL_BUTTON = 76.0f;
constexpr float DELETE_BUTTON = 108.0f;
constexpr float MENU_BUTTON_WIDTH = 240.0f;
constexpr float FORM_WIDTH = 480.0f;
constexpr float CHROME_HEIGHT = 260.0f; // Title and buttons around the list.
constexpr float CARD_HEIGHT_ESTIMATE = 64.0f;

std::string Trim(const std::string& text)
{
    size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// Distros that can be created: they have a URL, and the file is a jar that runs by itself
// (NeoForge's is an installer).
std::vector<ServerDistro> SelectableDistros()
{
    std::vector<ServerDistro> distros;
    for (const auto& [distro, url] : serverDownloads)
    {
        if (!url.empty() && distro != ServerDistro::NEOFORGE) distros.push_back(distro);
    }
    return distros;
}

// A download running on a worker thread; the screen reads it each frame.
struct Download {
    std::mutex mutex;
    uint64_t received = 0;
    uint64_t total = 0;
    bool done = false;
    std::string error;
};

struct WorldScreen {
    enum class Mode { LIST, CREATE, EDIT };

    GlobalState* state = nullptr;
    Mode mode = Mode::LIST;
    std::string error;

    // Elements of the current build that update reacts to.
    struct Row {
        Pincel::Element play;
        Pincel::Element edit;
        Pincel::Element remove;
    };
    std::vector<Row> rows;
    int confirmingDelete = -1;
    Pincel::Element createButton;
    Pincel::Element backButton;
    Pincel::Element errorText;

    // The create and edit forms.
    World draft;
    size_t distroIndex = 0;
    int editing = -1;
    std::string javaPath;
    std::string javaArgs;
    Pincel::Element nameInput;
    Pincel::Element javaPathInput;
    Pincel::Element javaArgsInput;
    Pincel::Element distroButton;
    Pincel::Element saveButton;
    Pincel::Element cancelButton;

    // While a new world's server is downloading.
    std::shared_ptr<Download> download;
    Pincel::Element progressText;
    std::string createdName;
};

std::string VersionLine(const World& world)
{
    return ServerDistroToString(world.distro) + " " + ServerVersion::ToString(world.version);
}

void BuildList(ControlRenderer& ui, WorldScreen& screen)
{
    using Pincel::Align;
    using Pincel::Color;
    using Pincel::Orientation;

    auto units = [&ui](float value) { return static_cast<uint16_t>(ui.Units(value)); };
    Pincel::Element column = ui.Column();
    const std::vector<World>& worlds = *GetWorlds();

    const int wanted = ui.Units(std::max<float>(1.0f, static_cast<float>(worlds.size())) * (CARD_HEIGHT_ESTIMATE + GAP));
    const int boxHeight = std::min(wanted, std::max(ui.Units(2.0f * CARD_HEIGHT_ESTIMATE), ui.Height() - ui.Units(CHROME_HEIGHT)));
    Pincel::Element box = column.AddStack(Orientation::Vertical);
    box.SetSize(static_cast<uint16_t>(ui.Width()), static_cast<uint16_t>(boxHeight))
        .SetAlign(Align::Center, Align::Start)
        .SetGap(units(GAP))
        .SetOverflow(Pincel::Overflow::Scroll);

    screen.rows.clear();
    if (worlds.empty())
    {
        box.SetAlign(Align::Center, Align::Center);
        box.AddText("No worlds yet. Create one below.").SetForeground(Color{ 153, 153, 153 });
    }
    for (size_t i = 0; i < worlds.size(); i++)
    {
        const World& world = worlds[i];
        Pincel::Element card = box.AddStack(Orientation::Horizontal);
        card.SetSize(units(CARD_WIDTH), 0)
            .SetAlign(Align::Start, Align::Center)
            .SetPadding(units(CARD_PADDING))
            .SetGap(units(GAP))
            .SetBackground(Color{ 16, 16, 16, 0.85f })
            .SetCornerRadius(units(CARD_RADIUS));

        Pincel::Element info = card.AddStack(Orientation::Vertical);
        info.SetSize(units(CARD_WIDTH - 2.0f * CARD_PADDING - 2.0f * SMALL_BUTTON - DELETE_BUTTON - 3.0f * GAP), 0);
        info.AddText(world.name);
        info.AddText(VersionLine(world)).SetForeground(Color{ 160, 160, 160 });

        WorldScreen::Row row;
        row.play = ui.AddButton(card, "Play", SMALL_BUTTON);
        row.edit = ui.AddButton(card, "Edit", SMALL_BUTTON);
        row.remove = ui.AddButton(card, screen.confirmingDelete == static_cast<int>(i) ? "Confirm" : "Delete", DELETE_BUTTON);
        screen.rows.push_back(row);
    }

    column.AddStack(Orientation::Vertical).SetSize(1, units(GAP));
    screen.errorText = column.AddText(screen.error);
    screen.errorText.SetForeground(Color{ 255, 102, 102 }).SetVisible(!screen.error.empty());

    column.AddStack(Orientation::Vertical).SetSize(1, units(8.0f));
    screen.createButton = ui.AddButton(column, "Create New World", MENU_BUTTON_WIDTH);
    screen.backButton = ui.AddButton(column, "Back", MENU_BUTTON_WIDTH);
}

void BuildForm(ControlRenderer& ui, WorldScreen& screen)
{
    using Pincel::Color;

    auto units = [&ui](float value) { return static_cast<uint16_t>(ui.Units(value)); };
    Pincel::Element column = ui.Column();
    const bool creating = screen.mode == WorldScreen::Mode::CREATE;

    if (screen.download)
    {
        column.AddText("Downloading " + ServerDistroToString(screen.draft.distro) + "...");
        screen.progressText = column.AddText("");
        screen.progressText.SetForeground(Color{ 160, 160, 160 });
        return;
    }

    auto addLabeled = [&](const char* label, const std::string& value, const char* hint)
    {
        column.AddText(label).SetForeground(Color{ 160, 160, 160 });
        Pincel::Element input = ui.AddField(column, FORM_WIDTH, hint);
        input.SetText(value);
        return input;
    };
    screen.nameInput = addLabeled("World name", screen.draft.name, "New world");
    if (creating)
    {
        column.AddText("Version: " + ServerVersion::ToString(screen.draft.version)).SetForeground(Color{ 160, 160, 160 });
        screen.distroButton = ui.AddButton(column, "Server: " + ServerDistroToString(screen.draft.distro), FORM_WIDTH);
    }
    else
    {
        screen.javaPathInput = addLabeled("Java path", screen.javaPath, "java");
        screen.javaArgsInput = addLabeled("Java arguments", screen.javaArgs, "");
    }

    column.AddStack(Pincel::Orientation::Vertical).SetSize(1, units(8.0f));
    Pincel::Element buttons = column.AddStack(Pincel::Orientation::Horizontal);
    buttons.SetGap(units(6.0f));
    screen.saveButton = ui.AddButton(buttons, creating ? "Create" : "Save", MENU_BUTTON_WIDTH / 2.0f);
    screen.cancelButton = ui.AddButton(buttons, "Cancel", MENU_BUTTON_WIDTH / 2.0f);

    screen.errorText = column.AddText(screen.error);
    screen.errorText.SetForeground(Color{ 255, 102, 102 }).SetVisible(!screen.error.empty());
}

void Build(ControlRenderer& ui, WorldScreen& screen)
{
    if (screen.mode == WorldScreen::Mode::LIST) BuildList(ui, screen);
    else BuildForm(ui, screen);
}

void BackToList(ControlRenderer& ui, WorldScreen& screen)
{
    screen.mode = WorldScreen::Mode::LIST;
    screen.download.reset();
    screen.editing = -1;
    ui.Rebuild();
}

// Makes the world's folder and entry, then fetches its jar on a worker thread.
void StartCreate(ControlRenderer& ui, WorldScreen& screen)
{
    std::string name = Trim(screen.draft.name);
    screen.draft.name = name.empty() ? "New world" : name;
    if (!CreateWorld(screen.draft))
    {
        screen.error = "Could not create the world folder";
        ui.Rebuild();
        return;
    }

    World world = GetWorlds()->back(); // Has its final name, if the one asked for was taken.
    screen.createdName = world.name;
    screen.download = std::make_shared<Download>();
    screen.error.clear();

    std::shared_ptr<Download> download = screen.download;
    std::thread([world, download]()
    {
        std::string error;
        DownloadServerJar(world, error, [download](uint64_t received, uint64_t total)
        {
            std::lock_guard<std::mutex> lock(download->mutex);
            download->received = received;
            download->total = total;
        });
        std::lock_guard<std::mutex> lock(download->mutex);
        download->error = error;
        download->done = true;
    }).detach();
    ui.Rebuild();
}

void UpdateDownload(ControlRenderer& ui, WorldScreen& screen)
{
    std::string text;
    bool done;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(screen.download->mutex);
        done = screen.download->done;
        error = screen.download->error;
        const uint64_t received = screen.download->received;
        const uint64_t total = screen.download->total;
        text = total > 0 ? std::to_string(received * 100 / total) + "%" : std::to_string(received / (1024 * 1024)) + " MB";
    }

    if (!done)
    {
        screen.progressText.SetText(text);
        return;
    }
    if (error.empty())
    {
        BackToList(ui, screen);
        return;
    }

    // Failed: drop the half-made world so the form can be tried again.
    DeleteWorld(screen.draft.dir, screen.createdName);
    screen.download.reset();
    screen.error = "Download failed: " + error;
    ui.Rebuild();
}

void UpdateForm(ControlRenderer& ui, WorldScreen& screen)
{
    if (screen.download)
    {
        UpdateDownload(ui, screen);
        return;
    }

    if (screen.nameInput.InputChanged()) screen.draft.name = screen.nameInput.GetText();
    if (screen.mode == WorldScreen::Mode::EDIT)
    {
        if (screen.javaPathInput.InputChanged()) screen.javaPath = screen.javaPathInput.GetText();
        if (screen.javaArgsInput.InputChanged()) screen.javaArgs = screen.javaArgsInput.GetText();
    }

    if (screen.mode == WorldScreen::Mode::CREATE && screen.distroButton.WasClicked())
    {
        ui.PlayClick();
        const std::vector<ServerDistro> distros = SelectableDistros();
        screen.distroIndex = (screen.distroIndex + 1) % distros.size();
        screen.draft.distro = distros[screen.distroIndex];
        ui.Rebuild();
        return;
    }

    if (screen.cancelButton.WasClicked())
    {
        ui.PlayClick();
        screen.error.clear();
        BackToList(ui, screen);
        return;
    }
    if (!screen.saveButton.WasClicked() && !screen.nameInput.InputSubmitted()) return;
    ui.PlayClick();

    if (screen.mode == WorldScreen::Mode::CREATE)
    {
        StartCreate(ui, screen);
        return;
    }

    // Edit: rename the folder with the world, then keep the Java settings.
    World& world = (*GetWorlds())[static_cast<size_t>(screen.editing)];
    std::string name = WorldStorage::SafeName(Trim(screen.draft.name));
    if (name != world.name)
    {
        const bool taken = std::any_of(GetWorlds()->begin(), GetWorlds()->end(), [&](const World& other)
        {
            return &other != &world && other.dir == world.dir && other.name == name;
        });
        std::error_code ec;
        if (taken) screen.error = "A world with that name already exists";
        else std::filesystem::rename(world.Path(), std::filesystem::path(world.dir) / name, ec);
        if (taken || ec)
        {
            if (!taken) screen.error = "Could not rename the folder: " + ec.message();
            ui.Rebuild();
            return;
        }
        world.name = name;
    }
    const std::string javaPath = Trim(screen.javaPath);
    world.jvmPath = javaPath.empty() ? "java" : javaPath; // Fall back to the system Java.
    world.jvmArgs = screen.javaArgs;
    screen.error = SaveWorlds() ? "" : "Could not save worlds.json";
    BackToList(ui, screen);
}

void UpdateList(ControlRenderer& ui, WorldScreen& screen, const WorldListActions& actions)
{
    std::vector<World>& worlds = *GetWorlds();

    for (size_t i = 0; i < screen.rows.size() && i < worlds.size(); i++)
    {
        if (screen.rows[i].play.WasClicked())
        {
            ui.PlayClick();
            const World& world = worlds[i];
            if (!std::filesystem::exists(world.Path() / "server.jar"))
            {
                screen.error = "'" + world.name + "' has no server.jar";
                ui.Rebuild();
            }
            else if (EnsureEula(world) && SetOfflineMode(world) && actions.play)
            {
                actions.play(world);
            }
            return;
        }
        if (screen.rows[i].edit.WasClicked())
        {
            ui.PlayClick();
            screen.mode = WorldScreen::Mode::EDIT;
            screen.editing = static_cast<int>(i);
            screen.draft = worlds[i];
            screen.javaPath = worlds[i].jvmPath;
            screen.javaArgs = worlds[i].jvmArgs;
            screen.error.clear();
            ui.Rebuild();
            return;
        }
        if (screen.rows[i].remove.WasClicked())
        {
            ui.PlayClick();
            if (screen.confirmingDelete == static_cast<int>(i))
            {
                DeleteWorld(worlds[i].dir, worlds[i].name);
                screen.confirmingDelete = -1;
            }
            else
            {
                screen.confirmingDelete = static_cast<int>(i); // Asks again before deleting the folder.
            }
            screen.error.clear();
            ui.Rebuild();
            return;
        }
    }

    if (screen.createButton.WasClicked())
    {
        ui.PlayClick();
        screen.mode = WorldScreen::Mode::CREATE;
        World::FromDefaults(&screen.draft, screen.state);
        const std::vector<ServerDistro> distros = SelectableDistros();
        screen.distroIndex = 0;
        for (size_t i = 0; i < distros.size(); i++)
        {
            if (distros[i] == screen.draft.distro) screen.distroIndex = i;
        }
        if (!distros.empty()) screen.draft.distro = distros[screen.distroIndex];
        screen.error.clear();
        ui.Rebuild();
    }
    else if (screen.backButton.WasClicked())
    {
        ui.PlayClick();
        if (actions.back) actions.back();
    }
}

} // namespace

bool DownloadServerJar(const World& world, std::string& error, const std::function<void(uint64_t, uint64_t)>& progress)
{
    const std::string url = GetServerDownloadURL(world.distro);
    if (url.empty())
    {
        error = ServerDistroToString(world.distro) + " has no download yet";
        return false;
    }

    std::vector<uint8_t> body;
    if (!Platform::OS::HttpGet(url, body, error, progress)) return false;

    std::ofstream file(world.Path() / "server.jar", std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    if (!file)
    {
        error = "Could not write server.jar";
        return false;
    }
    return true;
}

void ConfigureWorldScreen(Screen* screen, GlobalState* state, WorldListActions actions)
{
    LoadWorlds(state);

    auto data = std::make_shared<WorldScreen>();
    data->state = state;
    screen->build = [data](ControlRenderer& ui) { Build(ui, *data); };
    screen->update = [data, actions](ControlRenderer& ui)
    {
        if (data->mode == WorldScreen::Mode::LIST) UpdateList(ui, *data, actions);
        else UpdateForm(ui, *data);
    };
}

} // namespace Volcano::GUI
