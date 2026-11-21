#pragma once

#include <charconv>
#include <map>
#include <string>
#include <string_view>
#include <system_error>

// The add-ons' settings file: "key=value" lines, the keys and values trimmed. What the values mean,
// and the visitor that applies them to a settings struct, stay with the add-on.
namespace fxcore::settings
{
using Values = std::map<std::string, std::string, std::less<>>;

std::string_view trim(std::string_view text);

// False for a line without '=' or without a key. An empty value is a value.
bool parseLine(std::string_view line, std::string& key, std::string& value);

// Every "key=value" line, a later key replacing an earlier one. Throws when the stream fails.
Values read(std::istream& input);

// The whole text as a number: trailing characters, an empty text or an overflow give false.
template<class T> bool number(std::string_view text, T& result)
{
    if (text.empty()) return false;

    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;

    result = value;

    return true;
}

// Each leaves the setting as it was when the text is not valid.
void readFlag(std::string_view text, bool& result);
// A finite number within [low, high]; outside it the value is clamped.
void readFloat(std::string_view text, float& result, float low, float high);
}
