#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "online_lobby.h"

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace eck {

// Adds the Online driver. `defer` runs a function on the UI thread outside the current frame.
void InstallOnlineMenu(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> defer);

// Called on the game thread, with Online play on, for the movies it opens, its screens' ActionScriptVoid calls and a
// race it launches.
void NoteOnlineMovie(std::string_view url);
void NoteOnlineScreenCall(uint32_t screen_class, uint32_t screen, std::string_view method);
void NoteOnlineRaceLaunch();
void NoteOnlineTravel();
void NoteOnlineMovieCall(std::string_view method, std::optional<int> number, std::string_view text);

// Between the UI thread that follows the session and the game thread that touches the game.
lobby::Bridge& OnlineLobby();

// Called on a guest thread when the main-menu movie loads (key ECK_ONLINE_MENU).
void NoteMainMenuShown();

}  // namespace eck
