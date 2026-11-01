#include "save_redirect.hpp"

#include "log.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace phantom_fx::save_redirect
{
namespace
{
// ISteamRemoteStorage (STEAMREMOTESTORAGE_INTERFACE_VERSION012, steam_api64.dll's): the slots of the
// calls the game makes, seen at its call sites (0x1401703ED write, 0x1407D487E read, 0x1407D4825
// delete, 0x1407D490B exists, 0x1407D494E size).
constexpr std::size_t FileWriteSlot = 0, FileReadSlot = 1, FileDeleteSlot = 3, FileExistsSlot = 10, GetFileSizeSlot = 12;
// ISteamUser (SteamUser017): GetUserDataFolder, which the game calls for the folder of its backup
// saves and display settings (0x140170537, 0x14054CBD5).
constexpr std::size_t GetUserDataFolderSlot = 6;

using GetInterface = void* (*)();
GetInterface originalGetStorage{}, originalGetUser{};
using GetUserDataFolder = bool (*)(void* user, char* buffer, int size);

// The game's save system asks for the user data folder once, as it starts, before the add-on loads
// (0x140170510 keeps it at + 0x20), so paths in the player's folder are redirected as files open too.
using CreateFile = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using DeleteFileWide = BOOL(WINAPI*)(LPCWSTR);
using DeleteFileNarrow = BOOL(WINAPI*)(LPCSTR);
using GetFileAttributes = DWORD(WINAPI*)(LPCWSTR);
using CreateDirectory = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using FindFirstFile = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
CreateFile originalCreateFile{};
DeleteFileWide originalDeleteFileWide{};
DeleteFileNarrow originalDeleteFileNarrow{};
GetFileAttributes originalGetFileAttributes{};
CreateDirectory originalCreateDirectory{};
FindFirstFile originalFindFirstFile{};

std::filesystem::path cloudFolder, userDataFolder;
// Steam's own answer, set once before `playerUserDataKnown`.
std::wstring playerUserData;
std::atomic<bool> playerUserDataKnown{}, isStorageRedirected{};
std::once_flag storagePatched, userPatched;

using Replacement = std::pair<std::size_t, void*>;

std::filesystem::path pathOf(const char* name)
{
    return cloudFolder / std::filesystem::path(name).filename();
}

bool fileWrite(void*, const char* name, const void* data, std::int32_t size)
{
    std::ofstream output(pathOf(name), std::ios::binary | std::ios::trunc);
    output.write(static_cast<const char*>(data), size);

    return output.good();
}

std::int32_t fileRead(void*, const char* name, void* data, std::int32_t size)
{
    std::ifstream input(pathOf(name), std::ios::binary);
    input.read(static_cast<char*>(data), size);

    return static_cast<std::int32_t>(input.gcount());
}

bool fileDelete(void*, const char* name)
{
    std::error_code error;

    return std::filesystem::remove(pathOf(name), error);
}

bool fileExists(void*, const char* name)
{
    std::error_code error;

    return std::filesystem::is_regular_file(pathOf(name), error);
}

std::int32_t getFileSize(void*, const char* name)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(pathOf(name), error);

    return error ? 0 : static_cast<std::int32_t>(size);
}

bool getUserDataFolder(void*, char* buffer, int size)
{
    const auto folder = userDataFolder.string();
    if (size <= 0 || folder.size() >= static_cast<std::size_t>(size)) return false;

    std::memcpy(buffer, folder.c_str(), folder.size() + 1);

    return true;
}

// `path` with the player's user data folder at its start replaced by the run's; empty for other paths.
// The game's paths may use either slash.
std::wstring redirected(LPCWSTR path)
{
    if (!path || !playerUserDataKnown.load(std::memory_order_acquire)) return {};

    const auto same = [](wchar_t a, wchar_t b) { return (a == L'/' ? L'\\' : towlower(a)) == (b == L'/' ? L'\\' : towlower(b)); };
    std::size_t length = 0;
    for (; length < playerUserData.size(); ++length)
        if (!path[length] || !same(path[length], playerUserData[length])) return {};

    if (path[length] && path[length] != L'\\' && path[length] != L'/') return {};

    return userDataFolder.wstring() + (path + length);
}

LPCWSTR either(const std::wstring& redirect, LPCWSTR path)
{
    return redirect.empty() ? path : redirect.c_str();
}

HANDLE WINAPI createFile(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile)
{
    const auto redirect = redirected(path);

    return originalCreateFile(either(redirect, path), access, share, security, disposition, flags, templateFile);
}

BOOL WINAPI deleteFileWide(LPCWSTR path)
{
    const auto redirect = redirected(path);

    return originalDeleteFileWide(either(redirect, path));
}

BOOL WINAPI deleteFileNarrow(LPCSTR path)
{
    const auto redirect = path ? redirected(std::filesystem::path(path).c_str()) : std::wstring();

    return redirect.empty() ? originalDeleteFileNarrow(path) : originalDeleteFileWide(redirect.c_str());
}

DWORD WINAPI getFileAttributes(LPCWSTR path)
{
    const auto redirect = redirected(path);

    return originalGetFileAttributes(either(redirect, path));
}

BOOL WINAPI createDirectory(LPCWSTR path, LPSECURITY_ATTRIBUTES security)
{
    const auto redirect = redirected(path);

    return originalCreateDirectory(either(redirect, path), security);
}

HANDLE WINAPI findFirstFile(LPCWSTR pattern, LPWIN32_FIND_DATAW found)
{
    const auto redirect = redirected(pattern);

    return originalFindFirstFile(either(redirect, pattern), found);
}

// Each interface is one object for the process, so its vtable is changed once.
bool patch(void* object, std::span<const Replacement> replacements, const char* what)
{
    auto** const table = *static_cast<void***>(object);
    const auto slots = std::ranges::max(replacements, {}, &Replacement::first).first + 1;
    DWORD protection{};
    if (!VirtualProtect(table, slots * sizeof(void*), PAGE_READWRITE, &protection))
    {
        log(std::string("The game's ") + what + " could not be redirected; this run may change the player's files.");
        return false;
    }

    for (const auto& [slot, function] : replacements) table[slot] = function;
    VirtualProtect(table, slots * sizeof(void*), protection, &protection);
    log(std::string("The game's ") + what + " are redirected.");

    return true;
}

void redirectStorage(void* storage)
{
    static const Replacement Replacements[]{{FileWriteSlot, reinterpret_cast<void*>(&fileWrite)}, {FileReadSlot, reinterpret_cast<void*>(&fileRead)},
        {FileDeleteSlot, reinterpret_cast<void*>(&fileDelete)}, {FileExistsSlot, reinterpret_cast<void*>(&fileExists)},
        {GetFileSizeSlot, reinterpret_cast<void*>(&getFileSize)}};

    if (patch(storage, Replacements, "Steam Cloud calls")) isStorageRedirected.store(true, std::memory_order_release);
}

void* getStorage()
{
    void* const storage = originalGetStorage();
    if (storage) std::call_once(storagePatched, redirectStorage, storage);

    return storage;
}

// Before its answer is replaced, Steam's own is kept to recognise the paths made from it.
void redirectUser(void* user)
{
    static const Replacement Replacements[]{{GetUserDataFolderSlot, reinterpret_cast<void*>(&getUserDataFolder)}};

    char folder[MAX_PATH]{};
    const auto steamsAnswer = reinterpret_cast<GetUserDataFolder>((*static_cast<void***>(user))[GetUserDataFolderSlot]);
    if (!steamsAnswer(user, folder, MAX_PATH) || !*folder)
    {
        log("Steam did not say where the player's user data folder is.");
        return;
    }

    playerUserData = std::filesystem::path(folder).wstring();
    if (patch(user, Replacements, "user data folder calls")) playerUserDataKnown.store(true, std::memory_order_release);
}

void* getUser()
{
    void* const user = originalGetUser();
    if (user) std::call_once(userPatched, redirectUser, user);

    return user;
}

template <typename Function>
bool hook(const wchar_t* module, const char* name, Function detour, Function& original)
{
    auto* const loaded = GetModuleHandleW(module);
    auto* const target = loaded ? reinterpret_cast<void*>(GetProcAddress(loaded, name)) : nullptr;
    auto status = target ? MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original)) : MH_ERROR_FUNCTION_NOT_FOUND;
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK) log(std::string("Hooking ") + name + " failed (" + MH_StatusToString(status) + ").");

    return status == MH_OK;
}
}

bool install(const std::filesystem::path& cloud, const std::filesystem::path& userData) noexcept
{
    cloudFolder = cloud;
    userDataFolder = userData;
    if (!hook(L"steam_api64.dll", "SteamRemoteStorage", &getStorage, originalGetStorage) || !hook(L"steam_api64.dll", "SteamUser", &getUser, originalGetUser))
        return false;

    // The game has already started Steam, so the player's folder is known before any file opens, and Steam Cloud is
    // redirected even if the game keeps the interface it already asked for.
    getUser();
    getStorage();
    if (!playerUserDataKnown.load(std::memory_order_acquire) || !isStorageRedirected.load(std::memory_order_acquire)) return false;

    if (!hook(L"kernel32.dll", "CreateFileW", &createFile, originalCreateFile) || !hook(L"kernel32.dll", "DeleteFileW", &deleteFileWide, originalDeleteFileWide)
        || !hook(L"kernel32.dll", "DeleteFileA", &deleteFileNarrow, originalDeleteFileNarrow)
        || !hook(L"kernel32.dll", "GetFileAttributesW", &getFileAttributes, originalGetFileAttributes)
        || !hook(L"kernel32.dll", "CreateDirectoryW", &createDirectory, originalCreateDirectory)
        || !hook(L"kernel32.dll", "FindFirstFileW", &findFirstFile, originalFindFirstFile))
        return false;

    log("The game's saves are read from and written to " + cloud.string() + ", its user data folder is " + userData.string() + ".");

    return true;
}
}
