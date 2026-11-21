#include "log.hpp"

#include <Windows.h>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <string>

namespace phantom_fx
{
namespace
{
std::mutex logMutex;
std::ofstream logFile;
std::mutex loggedOnceMutex;
std::set<std::string, std::less<>> loggedOnce;

bool writtenSinceProcessStart(const std::filesystem::path& file)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)
        || !GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &attributes))
        return false;

    return CompareFileTime(&attributes.ftLastWriteTime, &created) >= 0;
}
}

// ReShade loads the add-on again whenever the game creates a new device after destroying the
// last one, so a log already written by this process is continued rather than replaced.
void openLog(const std::filesystem::path& file)
{
    const auto mode = writtenSinceProcessStart(file) ? std::ios::app : std::ios::trunc;
    std::lock_guard lock(logMutex);
    logFile = std::ofstream(file, mode);
}

void log(std::string_view message)
{
    std::lock_guard lock(logMutex);
    if (logFile) logFile << GetTickCount64() << " [thread " << GetCurrentThreadId() << "] " << message << std::endl;
}

void logOnce(std::string_view message)
{
    std::lock_guard lock(loggedOnceMutex);
    if (loggedOnce.emplace(message).second) log(message);
}

std::string hex(std::uint64_t value)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}
}
