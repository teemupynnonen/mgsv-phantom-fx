#include "devkit/commands.hpp"

#include "devkit/log.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

namespace devkit::commands
{
namespace
{
// A directory scan every frame start would find nothing most of the time.
constexpr std::uint32_t PollFrames = 6;

std::filesystem::path commandsFolder; // Empty while off
Run runLine{};
Waiting waitingLine{};
ScriptStarted started{};
std::atomic<std::uint64_t> presented{};
std::uint32_t framesSincePoll{};
// The command file running, on the game thread only.
std::optional<Script> running;

std::mutex jobMutex;
std::deque<PresentJob> jobs; // Run one a present, in the order asked
std::string jobResults;      // Every answer so far, for the line that asked
bool jobWaiting{};           // The game thread waits for the jobs

std::optional<std::filesystem::path> nextCommandFile()
{
    std::error_code error;
    std::optional<std::filesystem::path> first;
    for (const auto& entry : std::filesystem::directory_iterator(commandsFolder, error))
        if (entry.path().extension() == ".cmd" && (!first || entry.path() < *first)) first = entry.path();

    return first;
}

// A file is claimed by a rename first, so a failed claim never runs its lines twice, and a file that did not read
// whole is not run at all.
std::optional<Script> load(const std::filesystem::path& file)
{
    auto claimed = file;
    claimed += ".running";
    std::error_code error;
    std::filesystem::rename(file, claimed, error);
    if (error) return std::nullopt;

    Script loaded{.result = std::filesystem::path(file).replace_extension(".result")};
    std::ifstream input(claimed);
    for (std::string line; std::getline(input, line);)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) loaded.lines.push_back(std::move(line));
    }
    const bool read = input.eof() && !input.bad();
    input.close();
    std::filesystem::remove(claimed, error);
    if (!read)
    {
        log("Command file " + file.filename().string() + " could not be read; it is skipped.");
        return std::nullopt;
    }

    return loaded;
}

// Written under another name first, so a reader never sees half of it. The key's script has no reader.
void finish(const Script& finished)
{
    if (finished.result.empty()) return;

    auto partial = finished.result;
    partial += ".partial";
    {
        std::ofstream file(partial, std::ios::binary);
        file << finished.output;
        file.close();
        if (file.fail())
        {
            log("Command result could not be written: " + partial.filename().string() + ".");
            return;
        }
    }

    std::error_code error;
    std::filesystem::rename(partial, finished.result, error);
    if (error) log("Command result could not be published: " + finished.result.filename().string() + ".");
}

// Whether the line that last ran still has the render thread, or the add-on, working for it; a line it
// gave up on says so.
bool waits(Script& current)
{
    if (current.waitUntil && presented.load(std::memory_order_relaxed) < *current.waitUntil) return true;

    current.waitUntil.reset();
    if (jobWaiting)
    {
        std::scoped_lock lock(jobMutex);
        if (!jobs.empty()) return true;

        current.output += jobResults;
        jobResults.clear();
        jobWaiting = false;
    }

    return waitingLine && waitingLine(current);
}

// `wait` is the runner's; every other line is the add-on's.
void runLineOf(Script& current, const std::string& line)
{
    // An indented line counts as one too.
    const auto trimmed = line.substr(line.find_first_not_of(" \t") == line.npos ? line.size() : line.find_first_not_of(" \t"));
    if (trimmed == "wait" || trimmed.starts_with("wait "))
    {
        std::uint64_t frames{};
        if (trimmed.size() > 5 && (std::istringstream(trimmed.substr(5)) >> frames)) current.waitUntil = presented.load(std::memory_order_relaxed) + frames;
        else current.output += "error: wait <frames>\n";
        return;
    }

    if (runLine) runLine(current, line);
    else current.output += "error: unknown command\n";
}

// Runs lines until one has to wait for frames or finishes the file.
void step(Script& current)
{
    while (!waits(current))
    {
        if (current.lines.empty())
        {
            finish(current);
            running.reset();
            return;
        }

        const auto line = std::move(current.lines.front());
        current.lines.pop_front();
        current.output += "> " + line + "\n";
        runLineOf(current, line);
    }
}
}

void install(std::filesystem::path folder, Run run, Waiting waiting, ScriptStarted onStarted) noexcept
{
    commandsFolder = std::move(folder);
    runLine = run;
    waitingLine = waiting;
    started = onStarted;
    running.reset();
    framesSincePoll = 0;
    presented.store(0, std::memory_order_relaxed);
    {
        std::scoped_lock lock(jobMutex);
        jobs.clear();
        jobResults.clear();
    }
    jobWaiting = false;
}

void onFrameStart() noexcept
{
    if (commandsFolder.empty()) return;

    try
    {
        if (!running)
        {
            if (++framesSincePoll < PollFrames) return;

            framesSincePoll = 0;
            if (const auto file = nextCommandFile()) running = load(*file);
            if (!running) return;

            if (started) started();
        }

        step(*running);
    }
    catch (const std::exception& error)
    {
        log(std::string("Command file: ") + error.what());
        if (running)
        {
            running->output += std::string("error: ") + error.what() + "\n";
            finish(*running);
        }

        running.reset();
    }
}

void onPresent(ID3D11DeviceContext* context, ID3D11Resource* backBuffer) noexcept
{
    if (commandsFolder.empty()) return;

    presented.fetch_add(1, std::memory_order_relaxed);

    std::scoped_lock lock(jobMutex);
    if (jobs.empty()) return;

    PresentJob asked = std::move(jobs.front());
    jobs.pop_front();

    try
    {
        jobResults += asked(context, backBuffer);
    }
    catch (const std::exception& error)
    {
        jobResults += std::string("error: ") + error.what() + "\n";
    }
}

void start(std::deque<std::string> lines) noexcept
{
    if (commandsFolder.empty() || running) return;

    running = Script{.lines = std::move(lines)};
    if (started) started();
}

bool isRunning() noexcept
{
    return running.has_value();
}

const std::filesystem::path& folder() noexcept
{
    return commandsFolder;
}

std::uint64_t presents() noexcept
{
    return presented.load(std::memory_order_relaxed);
}

void askPresent(PresentJob asked) noexcept
{
    std::scoped_lock lock(jobMutex);
    jobs.push_back(std::move(asked));
    jobWaiting = true;
}

std::string timeNow()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);

    return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
}
}
