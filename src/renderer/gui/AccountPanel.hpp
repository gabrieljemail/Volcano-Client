#pragma once
#ifndef VOLCANO_GUI_ACCOUNT_PANEL_H
#define VOLCANO_GUI_ACCOUNT_PANEL_H

#include <string>
#include <vector>
#include <Pincel/Pincel.hpp>

namespace Volcano {

class MSAccountManager;

namespace GUI {

class ControlRenderer;

// The account button in the top right of the menus, drawn by Pincel over a Screen: shows who connections log in as,
// and opens a list to switch between the signed-in Microsoft accounts, go back to offline mode, remove one, or add
// another through the device-code sign-in. Built by ControlRenderer with each screen, and rebuilt when what it shows
// changes. Render thread only.
class AccountPanel
{
public:
    void Build(ControlRenderer& ui, MSAccountManager* accounts);

    // Reacts to clicks and asks `ui` to rebuild when the accounts or the sign-in changed. `visible` is false in a world.
    void Update(ControlRenderer& ui, MSAccountManager* accounts, bool visible);

private:
    std::string Signature(MSAccountManager* accounts) const;

    bool open = false;
    std::string builtSignature;

    Pincel::Element root;
    Pincel::Element toggleButton;
    Pincel::Element offlineButton;
    Pincel::Element addButton;
    Pincel::Element cancelButton;
    Pincel::Element copyCodeButton;
    Pincel::Element copyPageButton;
    std::vector<Pincel::Element> selectButtons;
    std::vector<Pincel::Element> removeButtons;
    std::string userCode;
    std::string verificationUri;
};

} // namespace GUI
} // namespace Volcano

#endif
