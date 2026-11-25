#include "dev/hot_kernels.hpp"

#include "log.hpp"

#include "gi_hot_kernels.h"
#include "gi_kernel_list.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace phantom_fx::hot_kernels
{
namespace
{
namespace fs = std::filesystem;

// Compiled results index the renderer's pipelines by Kernel: both lists must come from the same GI_KERNELS.
static_assert(std::size(HotKernelList) == static_cast<std::size_t>(gi_d3d12::Kernel::Count));

// Per process, so two copies of the game (the player's and the automation's) cannot trade DXIL for the same kernel name.
const std::wstring Unique = std::to_wstring(GetCurrentProcessId());

std::mutex mutex;
std::mutex starting; // Guards `worker` between start and stop.
std::vector<Compiled> published;             // Not yet taken.
std::map<std::size_t, std::vector<std::uint8_t>> latest; // Every kernel's last good DXIL, for a renderer that starts again.
std::jthread worker;

fs::path outputFolder()
{
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    fs::path folder = fs::path(temp) / L"PhantomFX-hot-kernels";
    std::error_code ignored;
    fs::create_directories(folder, ignored);
    return folder;
}

// A build's files, and every earlier session's: a day old and nobody's running with them.
void clearStale(const fs::path& folder)
{
    std::error_code ignored;
    const auto yesterday = fs::file_time_type::clock::now() - std::chrono::hours{24};
    for (fs::directory_iterator it{folder, ignored}, end; !ignored && it != end; it.increment(ignored))
    {
        std::error_code error;
        if (fs::last_write_time(it->path(), error) < yesterday && !error) fs::remove(it->path(), error);
    }
}

fs::file_time_type modified(const fs::path& file)
{
    std::error_code error;
    const auto time = fs::last_write_time(file, error);
    return error ? fs::file_time_type{} : time;
}

// When this add-on was built: a source saved after that is newer than the DXIL the add-on carries.
fs::file_time_type builtAt()
{
    HMODULE self{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&builtAt), &self))
        return {};
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return {};
    return modified(path);
}

std::string narrow(const std::wstring& text)
{
    return fs::path(text).string();
}

struct Running
{
    std::size_t kernel{};
    PROCESS_INFORMATION process{};
    fs::path dxil, errors;
};

// Starts DXC on one kernel as the build does (CMakeLists.txt, add_gi_kernels), its messages into a file beside the DXIL.
bool launch(std::size_t kernel, const fs::path& folder, Running& running)
{
    const auto& row = HotKernelList[kernel];
    running.kernel = kernel;
    running.dxil = folder / (std::wstring(row.entry) + L"-" + Unique + L".dxil");
    running.errors = folder / (std::wstring(row.entry) + L"-" + Unique + L".txt");
    std::error_code ignored;
    fs::remove(running.dxil, ignored);

    std::wstring command = std::format(LR"("{}" -nologo -T cs_6_5 -E {} -HV 2021 -O3 -WX)", HotDxc, row.entry);
    for (const auto* include : HotIncludes) command += std::format(LR"( -I "{}")", include);
    command += std::format(LR"( -Fo "{}" "{}")", running.dxil.wstring(), row.source);

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE errors = CreateFileW(running.errors.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (errors == INVALID_HANDLE_VALUE)
    {
        const auto error = GetLastError();
        log(std::format("Hot kernels: {} not compiled, the diagnostics file {} could not be opened ({}).", narrow(row.entry),
            running.errors.string(), error));
        return false;
    }

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = errors;
    const bool ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &running.process) != 0;
    CloseHandle(errors);
    if (!ok) log(std::format("Hot kernels: DXC did not start for {} ({}).", narrow(row.entry), GetLastError()));
    return ok;
}

// Compiles `kernels` side by side and publishes those that compiled. A stop request ends the wait, so an unload is not
// held up by a save in flight.
void compile(const std::set<std::size_t>& kernels, const fs::path& folder, const std::stop_token& stop)
{
    const auto began = std::chrono::steady_clock::now();
    std::vector<Running> runs;
    for (const auto kernel : kernels)
    {
        if (stop.stop_requested()) break;
        Running running;
        if (launch(kernel, folder, running)) runs.push_back(running);
    }

    std::vector<Compiled> fresh;
    std::string failed;
    for (auto& running : runs)
    {
        while (WaitForSingleObject(running.process.hProcess, 50) == WAIT_TIMEOUT && !stop.stop_requested()) {}
        const bool stopped = stop.stop_requested();
        if (stopped) TerminateProcess(running.process.hProcess, 0);
        WaitForSingleObject(running.process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(running.process.hProcess, &exitCode);
        CloseHandle(running.process.hProcess);
        CloseHandle(running.process.hThread);
        if (stopped) continue;

        const auto entry = narrow(HotKernelList[running.kernel].entry);
        std::ifstream dxil(running.dxil, std::ios::binary);
        if (exitCode != 0 || !dxil)
        {
            std::ifstream errors(running.errors);
            const std::string text{std::istreambuf_iterator<char>(errors), {}};
            log(std::format("Hot kernels: {} did not compile; the kernel in use stays.\n{}", entry, text.substr(0, 4000)));
            failed += failed.empty() ? entry : ", " + entry;
            continue;
        }
        fresh.push_back({running.kernel, {std::istreambuf_iterator<char>(dxil), {}}});
    }

    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    log(std::format("Hot kernels: {} of {} compiled in {:.1f} s{}.", fresh.size(), kernels.size(), seconds,
        failed.empty() ? "" : "; failed: " + failed));
    if (fresh.empty()) return;

    std::lock_guard lock(mutex);
    for (auto& each : fresh)
    {
        latest[each.kernel] = each.code;
        std::erase_if(published, [&](const Compiled& older) { return older.kernel == each.kernel; });
        published.push_back(std::move(each));
    }
}

void watch(std::stop_token stop)
{
    const auto folder = outputFolder();
    clearStale(folder);
    // Each file watched, and the kernels a change to it compiles again: a kernel's own source its kernels, an include all of them.
    std::map<fs::path, std::set<std::size_t>> touches;
    std::set<std::size_t> every;
    for (std::size_t kernel = 0; kernel < std::size(HotKernelList); ++kernel)
    {
        touches[fs::path(HotKernelList[kernel].source)].insert(kernel);
        every.insert(kernel);
    }
    for (const auto* file : HotWatched) touches[fs::path(file)].insert(every.begin(), every.end());

    std::map<fs::path, fs::file_time_type> seen;
    for (const auto& [file, kernels] : touches) seen[file] = modified(file);
    log(std::format("Hot kernels: watching {} files for {} kernels.", touches.size(), every.size()));

    // What was saved since this add-on was built (the renderer starts from its DXIL): compile it now, not at the next save.
    if (const auto built = builtAt(); built != fs::file_time_type{})
    {
        std::set<std::size_t> newer;
        for (const auto& [file, time] : seen)
        {
            if (time <= built) continue;
            log("Hot kernels: " + file.filename().string() + " is newer than the build.");
            newer.insert(touches[file].begin(), touches[file].end());
        }
        if (!newer.empty()) compile(newer, folder, stop);
    }

    while (!stop.stop_requested())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        std::set<std::size_t> changed;
        for (auto& [file, time] : seen)
        {
            const auto now = modified(file);
            if (now == time) continue;
            time = now;
            log("Hot kernels: " + file.filename().string() + " changed.");
            changed.insert(touches[file].begin(), touches[file].end());
        }
        if (changed.empty()) continue;

        // An editor saves in steps: let it finish.
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        compile(changed, folder, stop);
    }
}
}

void start() noexcept
{
    try
    {
        std::lock_guard lock(starting);
        if (worker.joinable()) return;
        worker = std::jthread(watch);
    }
    catch (...)
    {
    }
}

void stop() noexcept
{
    // Joined here, on the caller's thread, not at process detach under the loader lock. A later start watches again.
    std::lock_guard lock(starting);
    if (!worker.joinable()) return;
    worker.request_stop();
    worker.join();
}

std::vector<Compiled> take() noexcept
{
    std::lock_guard lock(mutex);
    return std::exchange(published, {});
}

std::vector<Compiled> replay() noexcept
{
    std::lock_guard lock(mutex);
    published.clear(); // They are in `latest` too.
    std::vector<Compiled> all;
    for (const auto& [kernel, code] : latest) all.push_back({kernel, code});
    return all;
}
}
