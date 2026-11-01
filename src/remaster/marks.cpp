#include "remaster/marks.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <format>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace phantom_fx::remaster
{
namespace
{
// Floats, so that a value reads back as the very number it was.
using Json = nlohmann::basic_json<nlohmann::ordered_map, std::vector, std::string, bool, std::int64_t, std::uint64_t, float>;

constexpr const char* KindNames[]{"other", "self-lit", "glass"};

struct NamedLocation
{
    std::uint16_t location;
    const char* name;
};

constexpr NamedLocation NamedLocations[]{{10, "Afghanistan"}, {20, "Africa"}, {50, "Mother Base"}};

constexpr std::size_t MostRegisterComponents = 32;
constexpr int SecondsInMinute = 60, MinutesInDay = 24 * 60;

std::uint16_t parsedCode(const Json& entry)
{
    const auto code = entry.get<std::int64_t>();
    if (code < 0 || code > std::numeric_limits<std::uint16_t>::max()) throw std::runtime_error("a code is out of range");

    return static_cast<std::uint16_t>(code);
}

std::uint64_t parsedFingerprint(const std::string& text)
{
    std::uint64_t value{};
    const auto* end = text.data() + text.size();
    const auto [at, error] = std::from_chars(text.data(), end, value, 16);
    if (error != std::errc{} || at != end) throw std::runtime_error("a fingerprint is not a hash");

    return value;
}

MaterialKind parsedKind(const std::string& text)
{
    const auto found = std::ranges::find(KindNames, text);
    if (found == std::end(KindNames)) throw std::runtime_error("no kind of material is called " + text);

    return static_cast<MaterialKind>(found - std::begin(KindNames));
}

MarkedMaterial parsedMaterial(const Json& entry)
{
    const auto name = hashOf(entry.at("name").get<std::string>());
    if (!name) throw std::runtime_error("a material's name is no hash");

    return {.name = *name,
        .fingerprint = parsedFingerprint(entry.at("fingerprint").get<std::string>()),
        .kind = parsedKind(entry.at("kind").get<std::string>()),
        .brightness = entry.at("brightness").get<float>(),
        .uses = entry.at("uses").get<std::int32_t>()};
}

Mark parsedMark(const Json& entry)
{
    const auto position = entry.at("position").get<std::vector<float>>();
    if (position.size() != 3) throw std::runtime_error("a position has three components");

    Mark mark{.location = parsedCode(entry.at("location")),
        .mission = parsedCode(entry.at("mission")),
        .position = {position[0], position[1], position[2]},
        .rotation = entry.at("rotation").get<float>(),
        .clock = entry.at("clock").get<float>()};
    for (const auto& material : entry.at("materials")) mark.materials.push_back(parsedMaterial(material));

    return mark;
}

Json materialJson(const MarkedMaterial& material)
{
    return {{"name", hashText(material.name)},
        {"fingerprint", std::format("{:016x}", material.fingerprint)},
        {"kind", KindNames[static_cast<std::size_t>(material.kind)]},
        {"brightness", material.brightness},
        {"uses", material.uses}};
}

Json markJson(const Mark& mark)
{
    Json materials = Json::array();
    for (const auto& material : mark.materials) materials.push_back(materialJson(material));

    return {{"location", mark.location},
        {"mission", mark.mission},
        {"position", mark.position},
        {"rotation", mark.rotation},
        {"clock", mark.clock},
        {"materials", std::move(materials)}};
}
}

ParsedMarks parseMarks(std::string_view text)
{
    const auto file = Json::parse(text, nullptr, false);
    if (file.is_discarded()) return {.error = "The file is not JSON."};
    if (!file.is_object() || !file.contains("marks") || !file["marks"].is_array()) return {.error = "The file has no list of marks."};

    try
    {
        std::vector<Mark> marks;
        for (const auto& mark : file["marks"]) marks.push_back(parsedMark(mark));

        return {.marks = std::move(marks)};
    }
    catch (const std::exception& error)
    {
        return {.error = error.what()};
    }
}

std::string writeMarks(const std::vector<Mark>& marks)
{
    Json list = Json::array();
    for (const auto& mark : marks) list.push_back(markJson(mark));

    Json file = Json::object();
    file["marks"] = std::move(list);

    return file.dump(2) + "\n";
}

std::string placeText(const Mark& mark)
{
    const auto known = std::ranges::find(NamedLocations, mark.location, &NamedLocation::location);
    const auto map = known != std::end(NamedLocations) ? std::string(known->name) : std::format("Location {}", mark.location);
    const auto& [x, y, z] = mark.position;

    return std::format("{}, at {}, {}, {}", map, std::lround(x), std::lround(y), std::lround(z));
}

std::string timeText(float clock)
{
    const int minutes = static_cast<int>(clock) / SecondsInMinute % MinutesInDay;
    return std::format("{:02}:{:02}", minutes / 60, minutes % 60);
}

std::uint64_t fingerprintOf(const PackedMaterial& material, std::uint64_t name, std::span<const Rule> rules)
{
    std::array<float, MostRegisterComponents> registers{};
    const auto count = std::min(material.registers.size(), registers.size());
    std::copy_n(material.registers.begin(), count, registers.begin());

    const PackedMaterial original{material.parameters, material.packing, {registers.data(), count}};
    for (const auto& rule : rulesOf(rules, name))
        if (applyRule(original, rule, Direction::Back)) break;

    // FNV-1a.
    std::uint64_t hash = 0xCBF29CE484222325;
    const auto add = [&](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001B3;
    };
    add(&name, sizeof(name));
    add(registers.data(), count * sizeof(float));

    return hash;
}
}
