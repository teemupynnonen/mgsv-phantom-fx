#include "core/settings/ini.hpp"

#include <algorithm>
#include <cmath>
#include <istream>
#include <stdexcept>
#include <utility>

namespace fxcore::settings
{
std::string_view trim(std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t\r");
    return begin == text.npos ? std::string_view{} : text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}

bool parseLine(std::string_view line, std::string& key, std::string& value)
{
    const auto separator = line.find('=');
    if (separator == line.npos) return false;

    const auto parsedKey = trim(line.substr(0, separator));
    if (parsedKey.empty()) return false;

    key = parsedKey;
    value = trim(line.substr(separator + 1));

    return true;
}

Values read(std::istream& input)
{
    Values values;
    std::string line;
    std::string key;
    std::string value;
    while (std::getline(input, line))
        if (parseLine(line, key, value)) values.insert_or_assign(std::move(key), std::move(value));

    if (input.bad()) throw std::runtime_error("Cannot read the settings file.");

    return values;
}

void readFlag(std::string_view text, bool& result)
{
    if (text == "0" || text == "1") result = text == "1";
}

void readFloat(std::string_view text, float& result, float low, float high)
{
    float parsed{};
    if (number(text, parsed) && std::isfinite(parsed)) result = std::clamp(parsed, low, high);
}
}
