#pragma once

#include <functional>

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace eck {

// Adds the Online menu driver. `defer` runs a function on the UI thread outside the current frame.
void InstallOnlineMenu(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> defer);

// Called on a guest thread when Free Race / Multiplayer is confirmed in the main menu (key ECK_ONLINE_ENTER).
void RequestOnlineMenu();

// Called on a guest thread when the main-menu movie loads (key ECK_ONLINE_MENU).
void NoteMainMenuShown();

}  // namespace eck
