#pragma once

#include <cstddef>
#include <cstdint>

// What DevKit.addon64 lets another add-on plug into it: its own lines for the command files, its own state, and what F9
// does. DevKit knows nothing of the add-ons that use this; PhantomFX is one of them (src/dev/devkit_link.cpp).
//
// An add-on finds the export once DevKit has loaded: GetProcAddress(GetModuleHandleW(L"DevKit.addon64"), "DevKitGetApi").
// DevKit loads first as a rule, so an add-on tries again at its first frames if the module is not there yet.
//
// Plain C types only: each add-on has its own CRT, so nothing that owns heap memory crosses. Text goes into a caller's
// buffer: the function returns the text's length, and writes as much as fits, with a NUL; a result of `cap` or more
// means the buffer was short and the call is made again with a larger one. DevKitNone says there is no answer. Every
// call below is for the game thread, except the present job's function, which runs on the render thread.
extern "C"
{
constexpr std::size_t DevKitNone = static_cast<std::size_t>(-1);
constexpr std::uint32_t DevKitApiVersion = 2;

// The command file running, as a line sees it. Valid for the call it is handed to.
struct DevKitScript;

// One line of a command file: its arguments, the text after the command's name.
using DevKitCommandFn = void (*)(DevKitScript* script, const char* arguments, void* user);
// Called each frame start after the line that asked, until it returns 0. `timedOut` is set once the presents it asked
// for have gone; it must then give up, say why and return 0.
using DevKitWaitFn = int (*)(DevKitScript* script, int timedOut, void* user);
// Text for `state` and `status`. For `state <name>` the case's folder (UTF-8) comes with it, for files that belong with
// the case; null otherwise.
using DevKitTextFn = std::size_t (*)(const char* caseFolder, char* out, std::size_t cap, void* user);
// Work for the render thread at its next present: the D3D11 device context and the swap chain's back buffer. What it
// returns is the line's output.
using DevKitPresentFn = std::size_t (*)(void* context, void* backBuffer, char* out, std::size_t cap, void* user);
// F9, while no script runs.
using DevKitKeyFn = void (*)(void* user);

struct DevKitApi
{
    std::uint32_t size, version;

    // The add-on's lines. A name is one word; a second add-on cannot take a name that is held (0 is returned).
    int (*addCommand)(const char* name, DevKitCommandFn fn, void* user);
    // What `state` and `status` add to their text.
    void (*addState)(DevKitTextFn fn, void* user);
    void (*addStatus)(DevKitTextFn fn, void* user);
    // What F9 does: it usually starts a script with `start`.
    void (*setKey)(DevKitKeyFn fn, void* user);

    // From a line, or from the wait it left.
    // Text for the line's output.
    void (*say)(DevKitScript* script, const char* text);
    // Lines, separated by newlines, that run next in the script, in this order.
    void (*queue)(DevKitScript* script, const char* lines);
    // Holds the lines after this one until `fn` returns 0, or `presents` presents have gone.
    void (*waitUntil)(DevKitScript* script, DevKitWaitFn fn, std::uint64_t presents, void* user);
    // Queues `fn` for the render thread: one job runs at each present, in the order asked, and the script waits until
    // all of its jobs have run. Their answers are the line's output, in that order.
    void (*presentJob)(DevKitScript* script, DevKitPresentFn fn, void* user);

    // From anywhere on the game thread.
    // A script of its own, newline separated, run from the next frame start, if none runs.
    void (*start)(const char* lines);
    std::uint64_t (*presents)();
    // The folder of the commands, cases, saves and files of the tools, UTF-8.
    std::size_t (*folder)(char* out, std::size_t cap);
    // tone: 0 a note, 1 done, 2 a warning, 3 an error.
    void (*toast)(int tone, const char* text, const char* detail);
    void (*log)(const char* text);

    // While the frame log records a frame (F10, or `frame`), its dump folder (UTF-8; made if missing), for an add-on's
    // own passes to save what they draw into; empty otherwise. Any thread: the render thread asks it.
    std::size_t (*recordingFolder)(char* out, std::size_t cap);
};

// DevKit's export: null before it is ready.
using DevKitGetApiFn = const DevKitApi* (*)();
}
