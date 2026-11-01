#include "dev/game_lua.hpp"

#include "device_check.hpp"
#include "game_code.hpp"

#include <Windows.h>
#include <mutex>
#include <utility>

namespace phantom_fx::game_lua
{
namespace
{
// The game's script system: the pointer at ScriptSystemRva, its lua_State at + 8. The game calls
// into Lua inside a scope: BeginScopeRva takes the system's lock (a reader lock at 0x2B64200), runs
// the system's queued work when asked to (0x6AD50; the game's per-frame callers ask) and notes the
// stack's top; EndScopeRva puts the top back when asked to and releases the lock.
constexpr std::uintptr_t ScriptSystemRva = 0x2B641F0, BeginScopeRva = 0x6A590, EndScopeRva = 0x6A960;
// Lua's own functions, from the base library's table at 0x24B6A70: loadstring calls luaL_loadbuffer,
// pcall calls lua_pcall and lua_pushboolean, luaL_checklstring calls lua_tolstring, tostring pushes
// "nil" with lua_pushlstring.
constexpr std::uintptr_t LoadBufferRva = 0x1A178E0, CallRva = 0x1A116C0, ToStringRva = 0x1A12150, PushStringRva = 0x1A118C0,
                         PushBooleanRva = 0x1A11750;

struct Scope
{
    std::uint8_t* system{};
    int top{};
    int restoreTop{};
};
using BeginScope = Scope* (*)(Scope* scope, bool restoreTop, bool runQueued);
using EndScope = void (*)(Scope* scope);
using LoadBuffer = int (*)(void* lua, const char* code, std::size_t size, const char* name);
using Call = int (*)(void* lua, int arguments, int results, int errorHandler);
using ToString = const char* (*)(void* lua, int index, std::size_t* length);
using PushString = void (*)(void* lua, const char* text, std::size_t length);
using PushBoolean = void (*)(void* lua, int value);

// The code read for each address above, checked before anything runs.
// mov [rsp + 8], rbx; mov [rsp + 0x18], rsi; push rdi; sub rsp, 0x20; movzx esi, r8b; mov rbx, rcx;
// mov dword ptr [rcx + 8], -1; mov dword ptr [rcx + 0xC], 0; test dl, dl; je; mov dword ptr [rcx + 0xC], 1;
// lea rdx, [rsp + 0x38]; lea rcx, [0x2B64200]; call; mov rdi, [ScriptSystemRva]
constexpr unsigned char BeginScopeCode[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x41,
    0x0F, 0xB6, 0xF0, 0x48, 0x8B, 0xD9, 0xC7, 0x41, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xC7, 0x41, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x84, 0xD2,
    0x74, 0x07, 0xC7, 0x41, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x54, 0x24, 0x38, 0x48, 0x8D, 0x0D, 0x35, 0x9C, 0xAF, 0x02, 0xE8,
    0x30, 0x9A, 0xFB, 0xFF, 0x48, 0x8B, 0x3D, 0x19, 0x9C, 0xAF, 0x02};
// sub rsp, 0x28; test byte ptr [rcx + 0xC], 1; mov rax, rcx; je; mov rcx, [rcx]; mov edx, [rax + 8];
// mov rcx, [rcx + 8]; call lua_settop; lea rdx, [rsp + 0x30]; lea rcx, [0x2B64200]; call; add rsp, 0x28; ret
constexpr unsigned char EndScopeCode[]{0x48, 0x83, 0xEC, 0x28, 0xF6, 0x41, 0x0C, 0x01, 0x48, 0x8B, 0xC1, 0x74, 0x0F, 0x48, 0x8B, 0x09,
    0x8B, 0x50, 0x08, 0x48, 0x8B, 0x49, 0x08, 0xE8, 0xF4, 0x75, 0x9A, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x48, 0x8D, 0x0D, 0x78, 0x98,
    0xAF, 0x02, 0xE8, 0x73, 0x97, 0xFB, 0xFF, 0x48, 0x83, 0xC4, 0x28, 0xC3};
// sub rsp, 0x38; mov [rsp + 0x20], rdx; mov [rsp + 0x28], r8; lea r8, [rsp + 0x20]; lea rdx, [getS];
// call lua_load; add rsp, 0x38; ret
constexpr unsigned char LoadBufferCode[]{0x48, 0x83, 0xEC, 0x38, 0x48, 0x89, 0x54, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x28, 0x4C, 0x8D,
    0x44, 0x24, 0x20, 0x48, 0x8D, 0x15, 0xA6, 0xF2, 0xFF, 0xFF, 0xE8, 0x01, 0x9C, 0xFF, 0xFF, 0x48, 0x83, 0xC4, 0x38, 0xC3};
// mov [rsp + 8], rbx; push rdi; sub rsp, 0x40; mov edi, r8d; mov r10d, edx; mov rbx, rcx; test r9d, r9d; jne
constexpr unsigned char CallCode[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x41, 0x8B, 0xF8, 0x44, 0x8B, 0xD2, 0x48,
    0x8B, 0xD9, 0x45, 0x85, 0xC9, 0x75, 0x04};
// mov [rsp + 8], rbx; mov [rsp + 0x10], rsi; push rdi; sub rsp, 0x20; mov rbx, r8; mov esi, edx; mov rdi, rcx; call index2adr
constexpr unsigned char ToStringCode[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49,
    0x8B, 0xD8, 0x8B, 0xF2, 0x48, 0x8B, 0xF9, 0xE8};
// mov [rsp + 8], rbx; mov [rsp + 0x10], rbp; mov [rsp + 0x18], rsi; push rdi; sub rsp, 0x20; mov r9, [rcx + 0x20] (the global state)
constexpr unsigned char PushStringCode[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
    0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x49, 0x20};
// mov r8, [rcx + 0x10] (the top); xor eax, eax; test edx, edx; setne al; mov dword ptr [r8 + 8], 1 (LUA_TBOOLEAN);
// mov [r8], eax; add qword ptr [rcx + 0x10], 0x10
constexpr unsigned char PushBooleanCode[]{0x4C, 0x8B, 0x41, 0x10, 0x33, 0xC0, 0x85, 0xD2, 0x0F, 0x95, 0xC0, 0x41, 0xC7, 0x40, 0x08, 0x01,
    0x00, 0x00, 0x00, 0x41, 0x89, 0x00, 0x48, 0x83};

const game_code::Expected Checked[]{{BeginScopeRva, BeginScopeCode}, {EndScopeRva, EndScopeCode}, {LoadBufferRva, LoadBufferCode}, {CallRva, CallCode},
    {ToStringRva, ToStringCode}, {PushStringRva, PushStringCode}, {PushBooleanRva, PushBooleanCode}};

// Called with the code, its chunk name and whether to try it as an expression first. While the code
// runs, print() collects lines instead of calling the game's. It returns the lines and the code's
// values as one string, or raises them with the error and its stack.
constexpr std::string_view Harness = R"(
return function(code, name, expression)
  local chunk = expression and loadstring("return " .. code, name)
  if not chunk then
    local problem
    chunk, problem = loadstring(code, name)
    if not chunk then error(problem, 0) end
  end

  local lines = {}
  local function add(...)
    local parts = {}
    for i = 1, select("#", ...) do parts[i] = tostring((select(i, ...))) end
    lines[#lines + 1] = table.concat(parts, "\t")
  end
  local function pack(...) return {n = select("#", ...), ...} end

  local debugLibrary = rawget(_G, "debug")
  local gamePrint = print
  print = add
  local results = pack(xpcall(chunk, debugLibrary and debugLibrary.traceback or tostring))
  print = gamePrint
  if not results[1] then
    lines[#lines + 1] = tostring(results[2])
    error(table.concat(lines, "\n"), 0)
  end

  if results.n > 1 then add(unpack(results, 2, results.n)) end
  return table.concat(lines, "\n")
end
)";

std::mutex mutex;
std::optional<std::string> pending;
Console published;
}

bool isSupported() noexcept
{
    static const bool expected = isSupportedGameBuild() && game_code::matches(Checked);

    return expected;
}

std::optional<Outcome> run(std::string_view code, std::string_view name, bool expression) noexcept
{
    if (!isSupported()) return std::nullopt;

    auto* const engine = game_code::image();
    auto* const system = *reinterpret_cast<std::uint8_t**>(engine + ScriptSystemRva);
    if (!system || !*reinterpret_cast<void**>(system + 8)) return std::nullopt;

    Scope scope;
    reinterpret_cast<BeginScope>(engine + BeginScopeRva)(&scope, true, true);
    void* const lua = *reinterpret_cast<void**>(scope.system + 8);
    int status = reinterpret_cast<LoadBuffer>(engine + LoadBufferRva)(lua, Harness.data(), Harness.size(), "=PhantomFX harness");
    if (status == 0) status = reinterpret_cast<Call>(engine + CallRva)(lua, 0, 1, 0);
    if (status == 0)
    {
        reinterpret_cast<PushString>(engine + PushStringRva)(lua, code.data(), code.size());
        reinterpret_cast<PushString>(engine + PushStringRva)(lua, name.data(), name.size());
        reinterpret_cast<PushBoolean>(engine + PushBooleanRva)(lua, expression);
        status = reinterpret_cast<Call>(engine + CallRva)(lua, 3, 1, 0);
    }
    const char* const text = reinterpret_cast<ToString>(engine + ToStringRva)(lua, -1, nullptr);
    Outcome outcome{.ran = status == 0, .text = text ? text : ""};
    reinterpret_cast<EndScope>(engine + EndScopeRva)(&scope);

    return outcome;
}

void submit(std::string code) noexcept
{
    std::scoped_lock lock(mutex);
    pending = std::move(code);
    published.waiting = true;
}

Console console() noexcept
{
    std::scoped_lock lock(mutex);

    return published;
}

void onFrameStart() noexcept
{
    std::optional<std::string> code;
    {
        std::scoped_lock lock(mutex);
        code = std::exchange(pending, std::nullopt);
    }
    if (!code) return;

    auto outcome = run(*code, "=console", true);
    if (!outcome) outcome = Outcome{.text = "The game's scripts are not running yet."};

    std::scoped_lock lock(mutex);
    published = {.waiting = pending.has_value(), .last = std::move(outcome), .runs = published.runs + 1};
}
}
