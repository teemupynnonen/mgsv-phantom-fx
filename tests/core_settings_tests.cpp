#include "core/settings/ini.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void keysAndValuesAreTrimmed()
{
    std::string key;
    std::string value;
    expect(fxcore::settings::parseLine("  Width\t= 1280  ", key, value), "Parse a padded line.");
    expect(key == "Width" && value == "1280", "Both sides are trimmed.");
}

void aLineNeedsAKeyAndAnEquals()
{
    std::string key;
    std::string value;
    expect(!fxcore::settings::parseLine("Width 1280", key, value), "Refuse a line without '='.");
    expect(!fxcore::settings::parseLine("=1280", key, value), "Refuse a line without a key.");
}

void anEmptyValueIsAValue()
{
    std::string key;
    std::string value = "kept";
    expect(fxcore::settings::parseLine("Width=", key, value), "An empty value is a value.");
    expect(key == "Width" && value.empty(), "The value is empty.");
}

void aLaterKeyReplacesAnEarlierOne()
{
    std::istringstream text("Width=1\nHeight=2\nWidth=3\n");
    const auto values = fxcore::settings::read(text);
    expect(values.size() == 2, "Two keys.");
    expect(values.at("Width") == "3", "The later value wins.");
}

void flagsTakeOnlyZeroOrOne()
{
    bool flag = false;
    fxcore::settings::readFlag("1", flag);
    expect(flag, "1 is on.");
    fxcore::settings::readFlag("0", flag);
    expect(!flag, "0 is off.");

    flag = true;
    fxcore::settings::readFlag("2", flag);
    expect(flag, "Anything else leaves the flag as it was.");
}

void floatsAreClampedToTheirRange()
{
    float value = 0.5f;
    fxcore::settings::readFloat("1e30", value, 0.0f, 1.0f);
    expect(value == 1.0f, "Above the range clamps to the top.");

    fxcore::settings::readFloat("-5", value, 0.0f, 1.0f);
    expect(value == 0.0f, "Below the range clamps to the bottom.");
}

void floatsRejectWhatIsNotANumber()
{
    float value = 0.25f;
    for (const char* text : {"", "1.0x", "nan", "inf", "--1"})
    {
        fxcore::settings::readFloat(text, value, 0.0f, 1.0f);
        expect(value == 0.25f, "Invalid text leaves the value as it was.");
    }
}

void numbersTakeTheWholeText()
{
    int value = 7;
    expect(fxcore::settings::number("42", value) && value == 42, "A whole number.");

    value = 7;
    expect(!fxcore::settings::number("42 ", value) && value == 7, "Trailing text is not a number.");
    expect(!fxcore::settings::number("", value), "Empty text is not a number.");
}
}

int main()
{
    try
    {
        keysAndValuesAreTrimmed();
        aLineNeedsAKeyAndAnEquals();
        anEmptyValueIsAValue();
        aLaterKeyReplacesAnEarlierOne();
        flagsTakeOnlyZeroOrOne();
        floatsAreClampedToTheirRange();
        floatsRejectWhatIsNotANumber();
        numbersTakeTheWholeText();
        std::cout << "Core settings tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
