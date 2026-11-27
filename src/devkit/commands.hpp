#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

struct ID3D11DeviceContext;
struct ID3D11Resource;

// Commands from files: `<folder>/<name>.cmd` runs, one line at a time, from the game thread at the main
// loop's frame start, and `<name>.result` holds what each line did once all have run. Files run in name
// order, one at a time; write each under another name first and rename it, so it is never read half
// written. The add-on registers what its lines do; `wait <frames>` is the runner's own.
namespace devkit::commands
{
// One line of a command file: the command and the rest after its name.
using Run = void (*)(struct Script& running, const std::string& line);
// Whether the line that last ran still waits for the add-on: true while it does.
using Waiting = bool (*)(struct Script& running);
// A new script's first line: the add-on's waits start over.
using ScriptStarted = void (*)();

// What the render thread does at its next present, and what it reports.
using PresentJob = std::function<std::string(ID3D11DeviceContext* context, ID3D11Resource* backBuffer)>;

// The command file running, on the game thread only.
struct Script
{
    std::filesystem::path result;
    std::deque<std::string> lines;
    std::string output;
    std::optional<std::uint64_t> waitUntil; // A present count
};

// `folder` is where the add-on's commands are read from; it outlives the call.
void install(std::filesystem::path folder, Run run, Waiting waiting, ScriptStarted started = nullptr) noexcept;
void onFrameStart() noexcept;
void onPresent(ID3D11DeviceContext* context, ID3D11Resource* backBuffer) noexcept;

// Starts a script at the game thread's next frame start; the add-on calls it from its own key.
void start(std::deque<std::string> lines) noexcept;
bool isRunning() noexcept;
// The folder the commands are read from; empty while off.
const std::filesystem::path& folder() noexcept;
// Presents since install, the frame count the waits count in.
std::uint64_t presents() noexcept;
// Has the render thread work for the line that asked: the runner waits for its result.
void askPresent(PresentJob job) noexcept;
// The local time as the tools name their files by it: 2026-11-01_21-05-09.
std::string timeNow();
}
