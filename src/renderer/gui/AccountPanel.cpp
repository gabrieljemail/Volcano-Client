#include "AccountPanel.hpp"
#include "ControlRenderer.hpp"
#include "Theme.hpp"
#include "../../network/SkinFetcher.hpp"
#include "../../network/microsoft/MSAccountManager.hpp"
#include <imgui.h>

namespace Volcano::GUI {

namespace
{
using Pincel::Align;
using Pincel::Element;
using Pincel::Orientation;
using Pincel::Position;
using namespace Theme;

constexpr float EDGE_MARGIN = 8.0f;
constexpr float ROW_WIDTH = 220.0f;
constexpr float REMOVE_WIDTH = 24.0f;
constexpr float HEAD_SIZE = 18.0f;
constexpr float HEAD_INSET = 8.0f;
constexpr float PANEL_PADDING = 8.0f;
constexpr float PANEL_GAP = 4.0f;
constexpr float FULL_WIDTH = ROW_WIDTH + REMOVE_WIDTH + PANEL_GAP; // A whole account row.
constexpr float PANEL_RADIUS = 12.0f;

bool Signing(const DeviceLoginState& login)
{
    return login.phase == DeviceLoginState::Phase::WaitingForCode || login.phase == DeviceLoginState::Phase::WaitingForUser
        || login.phase == DeviceLoginState::Phase::SigningIn;
}
} // namespace

std::string AccountPanel::Signature(MSAccountManager* accounts, SkinFetcher* skins) const
{
    std::string text = open ? "o" : "c";
    std::optional<size_t> current = accounts->CurrentIndex();
    text += current.has_value() ? std::to_string(*current) : "-";
    std::vector<uint8_t> head;
    for (const MicrosoftAccount& account : accounts->Snapshot())
    {
        text += "|" + account.GetProfileName();
        if (skins != nullptr && skins->TryGetHead("head:" + account.GetUuid(), head)) text += "+"; // Its face arrived.
    }

    DeviceLoginState login = accounts->GetDeviceLogin();
    text += "#" + std::to_string(static_cast<int>(login.phase)) + login.userCode + login.error;
    return text;
}

void AccountPanel::Build(ControlRenderer& ui, MSAccountManager* accounts, SkinFetcher* skins)
{
    selectButtons.clear();
    removeButtons.clear();
    toggleButton = offlineButton = addButton = cancelButton = copyCodeButton = copyPageButton = Element();
    root = Element();
    if (accounts == nullptr) return;

    std::vector<MicrosoftAccount> list = accounts->Snapshot();
    std::optional<size_t> current = accounts->CurrentIndex();
    DeviceLoginState login = accounts->GetDeviceLogin();
    builtSignature = Signature(accounts, skins);
    userCode = login.userCode;
    verificationUri = login.verificationUri;

    auto units = [&ui](float value) { return static_cast<std::uint16_t>(ui.Units(value)); };

    // The account's face at the left edge of `button`; nothing until it downloads (or if the account has no skin).
    auto AddHead = [&](Element button, const MicrosoftAccount& account)
    {
        std::vector<uint8_t> pixels;
        const std::string key = "head:" + account.GetUuid();
        if (skins != nullptr && !account.GetSkinUrl().empty())
        {
            skins->Request(key, account.GetSkinUrl(), account.GetSkinSlim());
            skins->TryGetHead(key, pixels);
        }
        if (pixels.empty()) return;
        // An overlay stretched over the button, so the face can be centered vertically at its left edge.
        Element overlay = button.AddStack(Orientation::Horizontal);
        overlay.SetPosition(Position::Absolute)
            .SetInsets(Pincel::Insets{ .left = 0, .top = 0, .right = 0, .bottom = 0 })
            .SetAlign(Align::Start, Align::Center)
            .SetPadding(0, 0, 0, units(HEAD_INSET));
        if (std::optional<Element> head = overlay.AddImage(pixels.data(), 8, 8))
        {
            head->SetSize(units(HEAD_SIZE), units(HEAD_SIZE)).SetImageFilter(Pincel::ImageFilter::Nearest);
        }
    };

    root = ui.Overlay().AddStack(Orientation::Vertical);
    root.SetPosition(Position::Fixed)
        .SetInsets(Pincel::Insets{ .top = ui.Units(EDGE_MARGIN), .right = ui.Units(EDGE_MARGIN) })
        .SetAlign(Align::End, Align::Start)
        .SetGap(units(PANEL_GAP));

    std::string label = current.has_value() && *current < list.size() ? list[*current].GetProfileName() : "Offline";
    toggleButton = ui.AddButton(root, label + (open ? " ^" : " v"), FULL_WIDTH);
    if (current.has_value() && *current < list.size()) AddHead(toggleButton, list[*current]);
    if (!open && !Signing(login)) return;

    Element panel = root.AddStack(Orientation::Vertical);
    panel.SetPadding(units(PANEL_PADDING))
        .SetGap(units(PANEL_GAP))
        .SetAlign(Align::Center, Align::Start)
        .SetBackground(PANEL_DARK)
        .SetBorder(units(2.0f), PANEL_LIGHT)
        .SetCornerRadius(units(PANEL_RADIUS));

    offlineButton = ui.AddButton(panel, current.has_value() ? "Offline" : "> Offline", FULL_WIDTH);
    for (size_t i = 0; i < list.size(); i++)
    {
        Element row = panel.AddStack(Orientation::Horizontal);
        row.SetGap(units(PANEL_GAP));
        selectButtons.push_back(ui.AddButton(row, (current == i ? "> " : "") + list[i].GetProfileName(), ROW_WIDTH));
        removeButtons.push_back(ui.AddButton(row, "x", REMOVE_WIDTH));
    }

    if (Signing(login))
    {
        if (login.phase == DeviceLoginState::Phase::WaitingForUser)
        {
            panel.AddText("Open " + login.verificationUri);
            panel.AddText("and enter: " + login.userCode);
            copyCodeButton = ui.AddButton(panel, "Copy code", FULL_WIDTH);
            copyPageButton = ui.AddButton(panel, "Copy page", FULL_WIDTH);
        }
        else
        {
            panel.AddText(login.phase == DeviceLoginState::Phase::SigningIn ? "Signing in..." : "Contacting Microsoft...");
        }
        cancelButton = ui.AddButton(panel, "Cancel", FULL_WIDTH);
    }
    else
    {
        if (login.phase == DeviceLoginState::Phase::Failed)
        {
            Element error = panel.AddText(login.error);
            error.SetSize(units(FULL_WIDTH), 0).SetForeground(Theme::Syntax::INVALID_WORD);
        }
        addButton = ui.AddButton(panel, "Add account", FULL_WIDTH);
    }
}

void AccountPanel::Update(ControlRenderer& ui, MSAccountManager* accounts, SkinFetcher* skins, bool visible)
{
    if (accounts == nullptr || !root.IsValid()) return;
    root.SetVisible(visible);
    if (!visible) return;

    bool changed = false;
    auto click = [&ui, &changed](Element& button)
    {
        if (!button.IsValid() || !button.WasClicked()) return false;
        ui.PlayClick();
        changed = true;
        return true;
    };

    if (click(toggleButton)) open = !open;
    if (click(offlineButton)) accounts->SetCurrent(std::nullopt);
    if (click(addButton)) accounts->BeginDeviceLogin();
    if (click(cancelButton)) accounts->CancelDeviceLogin();
    if (click(copyCodeButton)) ImGui::SetClipboardText(userCode.c_str());
    if (click(copyPageButton)) ImGui::SetClipboardText(verificationUri.c_str());

    // At most one of these happens per click, and a removal shifts the rows, so act on the first.
    for (size_t i = 0; i < removeButtons.size(); i++)
    {
        if (click(removeButtons[i]))
        {
            accounts->Remove(i);
            break;
        }
        if (click(selectButtons[i]))
        {
            accounts->SetCurrent(i);
            break;
        }
    }

    if (changed || Signature(accounts, skins) != builtSignature) ui.Rebuild();
}

} // namespace Volcano::GUI
