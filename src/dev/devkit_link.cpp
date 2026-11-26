// PhantomFX's lines for the DevKit add-on's command files (devkit/extension.hpp): the camera held at a pose, the
// records, the costs, the probes, the scene, the settings and the sky, which run inside its passes. DevKit owns the
// runner, F9 and the waits and knows nothing of these; PhantomFX plugs them in once DevKit has loaded, and without it
// there are no lines.
#include "dev/devkit_link.hpp"

#include "core.hpp"
#include "dev/game_shadow.hpp"
#include "dev/held_camera.hpp"
#include "dev/inspect_trace.hpp"
#include "dev/perf_record.hpp"
#include "dev/remaster/tool.hpp"
#include "devkit/commands.hpp"
#include "devkit/extension.hpp"
#include "features/global_illumination/river_light.hpp"
#include "game_loop.hpp"
#include "log.hpp"
#include "passes/ray_tracing/engine_scene.hpp"
#include "passes/ray_tracing/gi_inputs.hpp"
#include "passes/ray_tracing/gi_renderer.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "resource_dump.hpp"
#include "weather.hpp"

#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace phantom_fx::devkit_link
{
namespace
{
// Presents a line waits for the render thread before it gives up: with frame generation several a game frame.
constexpr std::uint64_t InspectionFrames = 3600, SceneFrames = 3600;
// Presents the scene's work to do stays the same before it counts as made: a model that never gets room
// would otherwise hold a case up for SceneFrames.
constexpr std::uint64_t SettledFrames = 60;
// The GPU's frames a `perf` line times without saying.
constexpr std::uint32_t DefaultCaptureFrames = 120;
// Frames between looks for DevKit while it is not there yet.
constexpr std::uint32_t LookFrames = 120;
constexpr int Done = 1;

const DevKitApi* kit{};
bool linked{};
std::uint32_t framesSinceLook{};
bool probeTurnedOnView{};

// The waits a line leaves; one script runs at a time, so each is one.
std::uint64_t inspectionsBefore{}, capturesBefore{}, sceneBusyAt{};
std::size_t sceneWorkSeen{};
std::string imagePath, imageName, texturesPath, texturesName, remasterText;

// Text into the caller's buffer: its whole length back, as much as fits written.
std::size_t give(std::string_view text, char* out, std::size_t cap) noexcept
{
    if (out && cap)
    {
        const auto count = std::min(text.size(), cap - 1);
        std::memcpy(out, text.data(), count);
        out[count] = '\0';
    }

    return text.size();
}

void say(DevKitScript* script, const std::string& text)
{
    kit->say(script, text.c_str());
}

void queue(DevKitScript* script, std::initializer_list<std::string> lines)
{
    std::string text;
    for (const auto& line : lines) text += line + "\n";
    kit->queue(script, text.c_str());
}

std::filesystem::path pathOf(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// DevKit's folder: the commands, the cases and the tools' files.
std::filesystem::path root()
{
    std::string text(512, '\0');
    auto length = kit->folder(text.data(), text.size());
    if (length >= text.size())
    {
        text.assign(length + 1, '\0');
        length = kit->folder(text.data(), text.size());
    }
    text.resize(std::min(length, text.size() - 1));

    return pathOf(text);
}

std::string utf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();

    return std::string(text.begin(), text.end());
}

std::string_view trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == text.npos) return {};

    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// PhantomFX.ini's value for `key`, if it has that line.
std::optional<std::string> iniValue(const Settings& settings, std::string_view key)
{
    std::istringstream lines(iniText(settings));
    for (std::string line; std::getline(lines, line);)
        if (line.starts_with(key) && line.size() > key.size() && line[key.size()] == '=') return line.substr(key.size() + 1);

    return std::nullopt;
}

// Whether two of a setting's values say the same: 1 and 1.000000 do.
bool isSameValue(std::string_view a, std::string_view b)
{
    if (a == b) return true;

    float first{}, second{};
    const auto [firstEnd, firstError] = std::from_chars(a.data(), a.data() + a.size(), first);
    const auto [secondEnd, secondError] = std::from_chars(b.data(), b.data() + b.size(), second);

    return firstError == std::errc{} && secondError == std::errc{} && firstEnd == a.data() + a.size() && secondEnd == b.data() + b.size() && first == second;
}

std::string text(const held_camera::Pose& pose)
{
    return std::format("{} {} {} {} {} {}", pose.position[0], pose.position[1], pose.position[2], pose.yaw, pose.pitch, pose.focalLength);
}

// A pose's five numbers; its focal length is read apart, as `move` puts its frames before it.
std::optional<held_camera::Pose> readPose(std::istream& numbers)
{
    held_camera::Pose pose{};
    if (numbers >> pose.position[0] >> pose.position[1] >> pose.position[2] >> pose.yaw >> pose.pitch) return pose;

    return std::nullopt;
}

// An optional last number: the focal length, or 0 (the game camera's lens) without one.
float readFocalLength(std::istream& numbers)
{
    float millimetres{};

    return numbers >> millimetres ? millimetres : 0.0f;
}

void showDebugView(DebugView view)
{
    auto diagnostics = core::currentDiagnostics();
    diagnostics.debugView = view;
    core::applyDiagnostics(diagnostics);
}

std::size_t sceneWork()
{
    return engine_scene::waitingFiles() + gi_renderer::waitingWork();
}

// The waits.
int pollInspection(DevKitScript* script, int timedOut, void*)
{
    if (inspect_trace::finished() != inspectionsBefore) return 0;
    if (!timedOut) return 1;

    say(script, "error: no record came: is the ray-traced scene running?\n");

    return 0;
}

int pollCapture(DevKitScript* script, int timedOut, void*)
{
    if (perf_record::capturesFinished() != capturesBefore)
    {
        say(script, perf_record::lastCapture() + "\n");
        return 0;
    }

    if (!timedOut) return 1;

    say(script, "error: no capture came: the GPU's frames were not timed\n");
    perf_record::cancelCapture();

    return 0;
}

int pollScene(DevKitScript* script, int timedOut, void*)
{
    if (const auto work = sceneWork(); work != sceneWorkSeen)
    {
        sceneWorkSeen = work;
        sceneBusyAt = kit->presents();
    }

    if (timedOut)
    {
        say(script, "error: the scene is still changing\n");
        return 0;
    }

    if (kit->presents() < sceneBusyAt + SettledFrames) return 1;
    if (sceneWorkSeen) say(script, std::format("the scene left {} things unmade\n", sceneWorkSeen));

    return 0;
}

// Render thread work.
std::size_t saveImage(void* context, void* backBuffer, char* out, std::size_t cap, void*)
{
    const bool isSaved = saveTexture(static_cast<ID3D11DeviceContext*>(context), static_cast<ID3D11Resource*>(backBuffer), DXGI_FORMAT_UNKNOWN, pathOf(imagePath));

    return give(isSaved ? "saved " + imageName + ".dds\n" : "error: the back buffer could not be saved\n", out, cap);
}

std::size_t saveRayTracingTextures(void* context, void*, char* out, std::size_t cap, void*)
{
    const auto path = pathOf(texturesPath);
    ray_tracing::saveTextures(static_cast<ID3D11DeviceContext*>(context), path);
    game_shadow::save(static_cast<ID3D11DeviceContext*>(context), path / "game-shadow.dds");

    return give("saved the textures into " + texturesName + "\n", out, cap);
}

std::size_t runRemaster(void*, void*, char* out, std::size_t cap, void*)
{
    return give(remaster::runLine(remasterText), out, cap);
}

// The lines.
void freeze(DevKitScript* script, std::string_view)
{
    if (const auto camera = held_camera::lastCamera()) held_camera::hold(*camera);
    else say(script, "error: no camera yet\n");
}

void releaseLine(DevKitScript*, std::string_view)
{
    held_camera::release();
    ray_tracing::clearProbe();
    if (std::exchange(probeTurnedOnView, false)) showDebugView(DebugView::FinalImage);
}

void cameraLine(DevKitScript* script, std::string_view rest)
{
    std::istringstream numbers{std::string(rest)};
    if (auto pose = readPose(numbers))
    {
        pose->focalLength = readFocalLength(numbers);
        held_camera::hold(*pose);
    }
    else say(script, "error: camera <x> <y> <z> <yaw> <pitch> [<mm>]\n");
}

void moveLine(DevKitScript* script, std::string_view rest)
{
    std::istringstream numbers{std::string(rest)};
    auto pose = readPose(numbers);
    int frames{};
    if (pose && numbers >> frames)
    {
        pose->focalLength = readFocalLength(numbers);
        held_camera::move(*pose, frames);
    }
    else say(script, "error: move <x> <y> <z> <yaw> <pitch> <frames> [<mm>]\n");
}

void liveLine(DevKitScript* script, std::string_view rest)
{
    if (rest == "0" || rest == "1") held_camera::setLive(rest == "1");
    else say(script, "error: live 0|1\n");
}

void waitSceneLine(DevKitScript* script, std::string_view)
{
    sceneWorkSeen = sceneWork();
    sceneBusyAt = kit->presents();
    kit->waitUntil(script, &pollScene, SceneFrames, nullptr);
}

void probe(DevKitScript* script, std::string_view rest)
{
    std::uint32_t x{}, y{};
    if (!(std::istringstream{std::string(rest)} >> x >> y))
    {
        say(script, "error: probe <x> <y>\n");
        return;
    }

    ray_tracing::probeAt(x, y);
    if (core::currentDiagnostics().debugView != DebugView::ShadowRays)
    {
        showDebugView(DebugView::ShadowRays);
        probeTurnedOnView = true;
    }

    // A readback's frames in flight before the text has this pixel.
    queue(script, {"wait 8", "probe_text"});
}

void probeText(DevKitScript* script, std::string_view)
{
    say(script, ray_tracing::shadowProbe() + "\n");
}

void owner(DevKitScript* script, std::string_view rest)
{
    std::uint64_t hash{};
    const auto hex = trimmed(rest);
    if (const auto [stop, error] = std::from_chars(hex.data(), hex.data() + hex.size(), hash, 16); hex.empty() || error != std::errc{} || !hash)
    {
        say(script, "error: owner <model hash, hex>\n");
        return;
    }

    engine_scene::watchOwners(hash);
    queue(script, {"wait 10", "owner_text"});
}

void ownerText(DevKitScript* script, std::string_view)
{
    const auto owners = engine_scene::watchedOwners();
    say(script, owners ? *owners : "error: no snapshot came: is the ray-traced scene running?\n");
}

void drawn(DevKitScript* script, std::string_view rest)
{
    std::istringstream numbers{std::string(rest)};
    float x{}, y{}, z{}, radius{};
    if (!(numbers >> x >> y >> z >> radius))
    {
        say(script, "error: drawn <x> <y> <z> <radius>\n");
        return;
    }

    gi_inputs::recordRigidDraws(true);
    queue(script, {"wait 10", "drawn_text " + std::string(rest)});
}

void drawnText(DevKitScript* script, std::string_view rest)
{
    std::istringstream numbers{std::string(rest)};
    float x{}, y{}, z{}, radius{};
    const auto draws = gi_inputs::takeRigidDraws();
    if (!(numbers >> x >> y >> z >> radius))
    {
        say(script, "error: drawn <x> <y> <z> <radius>\n");
        return;
    }

    // Each place and mesh once, with how many draws of it the frames had.
    std::map<std::tuple<long, long, long, std::uint32_t, std::uint32_t>, int> seen;
    for (const auto& draw : draws)
    {
        const float dx = draw.position[0] - x, dy = draw.position[1] - y, dz = draw.position[2] - z;
        if (dx * dx + dy * dy + dz * dz > radius * radius) continue;

        ++seen[{std::lround(draw.position[0] * 100), std::lround(draw.position[1] * 100), std::lround(draw.position[2] * 100), draw.firstIndex, draw.indexCount}];
    }

    for (const auto& [key, count] : seen)
    {
        const auto& [px, py, pz, first, indices] = key;
        say(script, std::format("({:.2f} {:.2f} {:.2f}) indices {} from {}: {} draws\n", px / 100.0, py / 100.0, pz / 100.0, indices, first, count));
    }

    if (seen.empty()) say(script, "no rigid draw there\n");
}

void inspect(DevKitScript* script, std::string_view rest)
{
    if (rest.empty())
    {
        say(script, "error: inspect <file>\n");
        return;
    }

    inspectionsBefore = inspect_trace::finished();
    kit->waitUntil(script, &pollInspection, InspectionFrames, nullptr);
    inspect_trace::request(root() / pathOf(rest));
}

void perf(DevKitScript* script, std::string_view rest)
{
    if (rest.empty())
    {
        say(script, "error: perf <file> [<frames>]\n");
        return;
    }

    // The frame count comes last, if at all: a case's name can have spaces.
    auto file = trimmed(rest);
    std::uint32_t frames = DefaultCaptureFrames;
    if (const auto space = file.find_last_of(" \t"); space != file.npos)
    {
        const auto* const end = file.data() + file.size();
        std::uint32_t asked{};
        if (const auto [stop, error] = std::from_chars(file.data() + space + 1, end, asked); error == std::errc{} && stop == end)
        {
            file = trimmed(file.substr(0, space));
            frames = asked;
        }
    }

    capturesBefore = perf_record::capturesFinished();
    kit->waitUntil(script, &pollCapture, frames + InspectionFrames, nullptr);
    perf_record::capture(root() / pathOf(file), frames);
}

void recordCosts(DevKitScript* script, std::string_view rest)
{
    const bool isOn = rest != "0";
    perf_record::setRecording(isOn);
    if (perf_record::isRecording() != isOn) say(script, "error: the recording switch could not be kept: PhantomFX.log says why\n");
}

void imageLine(DevKitScript* script, std::string_view rest)
{
    if (rest.empty())
    {
        say(script, "error: image <file>\n");
        return;
    }

    imageName = std::string(rest);
    imagePath = utf8(root() / pathOf(std::string(rest) + ".dds"));
    kit->presentJob(script, &saveImage, nullptr);
}

void textures(DevKitScript* script, std::string_view rest)
{
    if (rest.empty())
    {
        say(script, "error: textures <folder>\n");
        return;
    }

    texturesName = std::string(rest);
    texturesPath = utf8(root() / pathOf(rest));
    kit->presentJob(script, &saveRayTracingTextures, nullptr);
}

void remasterLine(DevKitScript* script, std::string_view rest)
{
    remasterText = std::string(rest);
    kit->presentJob(script, &runRemaster, nullptr);
}

void gameShadow(DevKitScript*, std::string_view rest)
{
    game_shadow::keep(rest != "0");
}

void enhancements(DevKitScript*, std::string_view rest)
{
    const bool isOn = rest != "0";
    if (core::enhancementsOn() != isOn) core::setEnhancements(isOn);
}

void reveal(DevKitScript* script, std::string_view rest)
{
    // The scene reads its models again with what it takes: restarted by the enhancements switch, and left on for the
    // record or inspect that follows.
    queue(script, {"wait_scene", "enhancements 0", "wait 5", "reveal_now " + std::string(rest), "enhancements 1", "wait_scene", "wait 45"});
}

void revealNow(DevKitScript*, std::string_view what)
{
    engine_scene::Reveal revealed;
    if (what == "all") revealed.everything = true;
    else if (what != "off")
    {
        std::istringstream hashes{std::string(what)};
        hashes >> std::hex;
        for (std::uint64_t hash{}; hashes >> hash;) revealed.hashes.push_back(hash);
    }

    engine_scene::setReveal(std::move(revealed));
}

void setting(DevKitScript* script, std::string_view rest)
{
    const auto separator = rest.find('=');
    const auto key = trimmed(rest.substr(0, separator));
    const auto value = separator == rest.npos ? std::string_view() : trimmed(rest.substr(separator + 1));
    const auto settings = core::currentSettings();
    const auto current = iniValue(settings, key);
    if (!current)
    {
        say(script, "error: no setting " + std::string(key) + "\n");
        return;
    }

    const auto changed = withLine(settings, key, value);
    core::applySettings(changed);

    // A test that changed nothing would read as costing nothing: asking for the value it has may be meant, a value
    // withLine turned down is not.
    if (changed != settings) return;

    if (isSameValue(*current, value)) say(script, "the setting stays as it was\n");
    else say(script, "error: " + std::string(key) + " does not take " + std::string(value) + "\n");
}

void diagnostic(DevKitScript* script, std::string_view rest)
{
    std::istringstream arguments{std::string(rest)};
    std::string name;
    int value{};
    auto diagnostics = core::currentDiagnostics();
    const std::pair<std::string_view, bool*> flags[]{{"show_scene", &diagnostics.showTracedScene}, {"two_sided_shadows", &diagnostics.twoSidedShadows},
        {"no_jitter", &diagnostics.noJitter}, {"main_view_sun", &diagnostics.mainViewCastsSun}};
    if (!(arguments >> name >> value))
    {
        say(script, "error: diagnostic debug_view|show_scene|two_sided_shadows|no_jitter|river_light <value>\n");
        return;
    }

    const auto* const flag = std::ranges::find(flags, name, &std::pair<std::string_view, bool*>::first);
    if (flag != std::end(flags)) *flag->second = value != 0;
    else if (name == "debug_view") diagnostics.debugView = static_cast<DebugView>(value);
    else if (name == "river_light")
    {
        river_light::setEnabled(value != 0);
        return;
    }
    else
    {
        say(script, "error: diagnostic debug_view|show_scene|two_sided_shadows|no_jitter|river_light <value>\n");
        return;
    }

    core::applyDiagnostics(diagnostics);
}

void clockLine(DevKitScript* script, std::string_view rest)
{
    std::istringstream arguments{std::string(rest)};
    float seconds{};
    std::string mode;
    if (arguments >> seconds)
    {
        weather::setClock(seconds);
        weather::stopClock(!(arguments >> mode && mode == "run"));
    }
    else say(script, "error: clock <seconds> [run]\n");
}

void weatherLine(DevKitScript* script, std::string_view rest)
{
    if (rest == "off")
    {
        weather::release();
        return;
    }

    std::istringstream arguments{std::string(rest)};
    int type{};
    float fog{};
    if (arguments >> type && type >= 0 && type < static_cast<int>(weather::Type::Count))
    {
        arguments >> fog;
        weather::force(static_cast<weather::Type>(type), fog);
    }
    else say(script, "error: weather <type> [<fog>]\n");
}

// A case: the frame as traced with what the game itself says, as the game draws it, then the scene with
// everything it leaves out, to name what the game drew and it lacks. The game stays frozen for more
// questions. tools/automation/case.py reads it.
void debugCase(DevKitScript* script, std::string_view rest)
{
    const auto name = rest.empty() ? ::devkit::commands::timeNow() : std::string(rest);
    const auto path = "cases/" + name;
    queue(script, {"freeze", "wait 30", "perf " + path + "/perf", "game_shadow 1", "wait 30", "state " + name, "inspect " + path + "/scene", "image " + path + "/traced",
        "textures " + path, "game_shadow 0", "enhancements 0", "wait 45", "image " + path + "/vanilla", "enhancements 1", "reveal all", "inspect " + path + "/revealed",
        "reveal off", "case_done " + name});
}

// F9: a case, or the game let go after one.
void caseKey(void*)
{
    if (held_camera::isHeld())
    {
        kit->toast(Done, "The game is let go", "");
        kit->start("release");
    }
    else
    {
        kit->toast(Done, "Debug case", "Hold on while it records: the picture changes a few times.");
        kit->start("case");
    }
}

// What the lines say.
std::size_t statusText(const char*, char* out, std::size_t cap, void*)
{
    const auto last = held_camera::lastCamera();
    const auto held = !held_camera::isHeld() ? 0 : held_camera::isLive() ? 2 : 1;
    auto line = std::format("frame {}, presented {}, {}, ", game_loop::currentFrame(), kit->presents(), held == 0 ? "running" : held == 2 ? "held live" : "frozen");
    line += last ? "camera " + text(*last) : std::string("no camera yet");

    return give(line + "\n" + ray_tracing::status(), out, cap);
}

// What the game is at, with the settings it ran on in a case's folder.
std::size_t stateText(const char* caseFolder, char* out, std::size_t cap, void*)
{
    const auto last = held_camera::lastCamera();
    const auto sky = weather::state();
    auto state = std::format("frame={}\ncamera={}\n", game_loop::currentFrame(), last ? text(*last) : std::string("?"));
    state += std::format("clock={}\nweather={} {}\n", sky.clock, sky.requested ? static_cast<int>(*sky.requested) : -1, sky.fogDensity);
    state += "scene=" + ray_tracing::status() + "\n";

    if (caseFolder)
    {
        std::error_code error;
        std::filesystem::copy_file(root().parent_path() / "PhantomFX.ini", pathOf(caseFolder) / "PhantomFX.ini", std::filesystem::copy_options::overwrite_existing, error);
    }

    return give(state, out, cap);
}

// A line's handler, with what it throws said as its output rather than carried into DevKit's frames.
template<void (*Handler)(DevKitScript*, std::string_view)> void handle(DevKitScript* script, const char* arguments, void*)
{
    try
    {
        Handler(script, arguments ? arguments : "");
    }
    catch (const std::exception& error)
    {
        say(script, std::string("error: ") + error.what() + "\n");
    }
}

void add(const char* name, DevKitCommandFn fn)
{
    if (!kit->addCommand(name, fn, nullptr)) log(std::string("DevKit link: the line ") + name + " is taken by another add-on.");
}

void plug()
{
    add("freeze", &handle<freeze>);
    add("release", &handle<releaseLine>);
    add("camera", &handle<cameraLine>);
    add("move", &handle<moveLine>);
    add("live", &handle<liveLine>);
    add("wait_scene", &handle<waitSceneLine>);
    add("probe", &handle<probe>);
    add("probe_text", &handle<probeText>);
    add("owner", &handle<owner>);
    add("owner_text", &handle<ownerText>);
    add("drawn", &handle<drawn>);
    add("drawn_text", &handle<drawnText>);
    add("inspect", &handle<inspect>);
    add("perf", &handle<perf>);
    add("record_costs", &handle<recordCosts>);
    add("image", &handle<imageLine>);
    add("textures", &handle<textures>);
    add("remaster", &handle<remasterLine>);
    add("game_shadow", &handle<gameShadow>);
    add("enhancements", &handle<enhancements>);
    add("reveal", &handle<reveal>);
    add("reveal_now", &handle<revealNow>);
    add("setting", &handle<setting>);
    add("diagnostic", &handle<diagnostic>);
    add("clock", &handle<clockLine>);
    add("weather", &handle<weatherLine>);
    add("case", &handle<debugCase>);

    kit->addState(&stateText, nullptr);
    kit->addStatus(&statusText, nullptr);
    kit->setKey(&caseKey, nullptr);
}

// DevKit's API, once DevKit has loaded: it loads first as a rule, so this is tried again at the first frames.
void link() noexcept
{
    const auto module = GetModuleHandleW(L"DevKit.addon64");
    if (!module) return;

    linked = true;
    const auto get = reinterpret_cast<DevKitGetApiFn>(GetProcAddress(module, "DevKitGetApi"));
    const auto* const api = get ? get() : nullptr;
    if (!api || api->version != DevKitApiVersion || api->size != sizeof(DevKitApi))
    {
        log("DevKit link: DevKit is not the version this PhantomFX knows; there are no debug lines.");
        return;
    }

    kit = api;
    plug();
    log("DevKit link: the debug lines are plugged into DevKit's command files.");
}
}

void install() noexcept
{
    link();
}

std::filesystem::path recordingDirectory() noexcept
{
    if (!kit || !kit->recordingFolder) return {};

    try
    {
        std::string text(512, '\0');
        auto length = kit->recordingFolder(text.data(), text.size());
        if (length >= text.size())
        {
            text.assign(length + 1, '\0');
            length = kit->recordingFolder(text.data(), text.size());
        }
        text.resize(std::min(length, text.size() - 1));

        return pathOf(text);
    }
    catch (const std::exception&)
    {
        return {};
    }
}

void onFrameStart() noexcept
{
    if (linked || ++framesSinceLook < LookFrames) return;

    framesSinceLook = 0;
    link();
}
}
