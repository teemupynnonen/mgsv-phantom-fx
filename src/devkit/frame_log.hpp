#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Development diagnostics of any MGSV add-on's rendering, the DevKit add-on's. The shaders the game creates are kept
// whatever else is on; the frame recording is on only when a `DevKit/frame-log` folder exists beside the add-ons as the
// game starts, and without it no recording event is registered, so that part costs nothing.
// They only read the game's Direct3D 11 state, so the frame renders unchanged. The files go into that folder:
// - Every shader the game creates is logged once to DevKit.log: stage, DXBC checksum, size. With a
//   `shaders` folder inside, the bytecode is saved there too.
// - F10 records a frame, present to present, to `frame-<n>.txt`: each draw, dispatch, clear and copy
//   with its shaders, targets, inputs and states, in the order the GPU runs them (command lists from
//   deferred contexts where they are executed). `frame-<n>-writers.txt` lists the call stacks that
//   wrote constant buffers.
// - `dump.txt`, lines of "<shader checksum> <count>", makes the first <count> calls with that shader
//   also record their constant buffers' contents and save their textures as DDS files in
//   `frame-<n>/`. A call inside a command list saves them once the list has run, so outputs show the
//   list's final state.
namespace devkit::frame_log
{
// Registers the ReShade events when `addonDirectory` holds the DevKit/frame-log folder.
void install(const std::filesystem::path& addonDirectory);
bool installed();
// Records a frame as F10 does, after its thirty warm-up presents (only with the frame-log folder). `recordings` counts
// those finished, and `lastRecording` is the name of the latest that was written: `frame-<n>`, empty after a failure.
// Records a frame as F10 does, after its thirty warm-up presents; its request ID, 0 when the log is off or one is
// already pending.
// `completedRequest` is the ID of the latest capture that finished: a capture asked for is done when they are equal.
std::uint64_t request();
std::uint64_t completedRequest();
std::uint64_t recordings();
std::string lastRecording();

// Every shader the game created since the add-on loaded, and how to take one apart. Kept whether or not the frame-log
// folder is there. `name` is its source file's, from its debug data, when it has one.
struct ShaderInfo
{
    std::string stage, hash, name;
    std::size_t size{};
};
std::vector<ShaderInfo> shaders(std::string_view filter = {});
std::size_t shaderCount();
// By a hash's start, or else a part of a source name. nullopt when no shader matches.
std::optional<std::string> disassemble(std::string_view hashOrName);
// Its bytecode (`.dxbc`) and its assembly (`.asm`) into DevKit/shaders; the bytecode's path.
std::optional<std::filesystem::path> saveShader(std::string_view hashOrName);
void uninstall();
// While F10 records a frame, its dump folder (made if missing), for the add-on's own passes to save
// what they draw from; empty otherwise.
std::filesystem::path recordingDirectory();
}
