#pragma once

#include <filesystem>

// Automation's saves (automation.hpp): an automated run must never touch the player's files. The game
// keeps its saves in Steam Cloud through ISteamRemoteStorage, and backups of them and its display
// settings in Steam's user data folder (ISteamUser::GetUserDataFolder). The Steam Cloud calls the game
// makes (write, read, delete, exists, size: vtable slots 0, 1, 3, 10 and 12 of version 012) read and
// write `cloud` instead, which holds a copy of a save to start from, and the user data folder is
// `userData`.
namespace phantom_fx::save_redirect
{
// Hooks steam_api64.dll's SteamRemoteStorage and SteamUser, with MinHook initialised, and logs what
// was done. False when the files could not be redirected.
bool install(const std::filesystem::path& cloud, const std::filesystem::path& userData) noexcept;
}
