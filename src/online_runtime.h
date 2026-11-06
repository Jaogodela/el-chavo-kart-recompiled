#pragma once

#include <filesystem>
#include <memory>

#include "online/game_socket.h"
#include "online/online_service.h"

namespace eck {

// Called when the launcher opens: the service then knows the user folder.
void ConfigureOnline(const std::filesystem::path& user_dir);

// The process-wide service. It outlives the launcher so a lobby survives Play.
online::OnlineService& Online();

// The launcher's edited (not yet saved) settings, applied right away.
void TickOnline(const online::UserSettings& settings);

// Once per presented frame while the game runs, with the saved settings. A no-op until ConfigureOnline.
void PumpOnlineService();

double OnlineNowSeconds();

// A last word to the service on the way out, when a session is running: true when it was told. The service works it out from
// the closed socket anyway, 30 s later.
bool LeaveOnlineForExit();

// The gameplay UDP socket, bound as soon as Online play is on and the game runs, so Windows Firewall can prompt right
// away. It then stays bound for the whole process: every room learns its public endpoint from it and plays over it.
// False while the bind fails (retried every few seconds).
bool EnsureOnlineGameSocket();
online::OnlineGameSocket& GameSocket();

}  // namespace eck
