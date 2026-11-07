// The remastering tool's panel: over the game at the screen's right edge, on its own keys, without ReShade's
// window. Only a dev build has this file (remaster.cmake).
#include "fxui/fxui.hpp"
#include "remaster/game_materials.hpp"
#include "remaster/in_force.hpp"
#include "dev/remaster/tool.hpp"

#include <Windows.h>
#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace phantom_fx::remaster
{
namespace
{
namespace colour = fxui::colour;

using game_materials::Material;

constexpr float PanelWidth = 404, LabelWidth = 96, Margin = 12, MostTuneShare = 0.55f, FontSize = 13;
constexpr ImU32 Ground = IM_COL32(0x0f, 0x11, 0x14, 0xf2);
constexpr int RightMouse = 1, MouseButtons = 5;
// The marks list shows at most this many rows: the lights below it are what gets worked on.
constexpr float MostMarkRows = 3;
// What [ and ] multiply a brightness by, and where ] starts from a light put out (a share of its original).
constexpr float BrightnessStep = 1.25f, FromOff = 0.01f;
// How far below its original a wide value goes. A self-lit material's intensity is what it looks like times the exposure:
// a quarantine platform ceiling tube of 221,182 stayed white down to about 200 and began to dim below 100 (2026-11-04).
constexpr float LeastShare = 1e-6f;
constexpr std::uint32_t FirstKey = 0x08, LastKey = 0xFE;

enum class Tab : std::uint8_t
{
    Marks,
    Rules
};

// What a mark's list shows: its lamps and glowing materials together, or its materials by kind.
enum class View : std::uint8_t
{
    Lights,
    Glass,
    All
};

// What the Rules tab lists: the rules against the file.
enum class RuleView : std::uint8_t
{
    All,
    Unsaved,
    Saved
};

// A row of the Lights list: a lamp or a material, for the arrow keys to walk.
struct Pick
{
    std::optional<LampRow> lamp;
    std::optional<MaterialRow> material;
};

bool isOpen{}, isMoving{}, isTyping{}, isBlocking{}, hasDrawnCursor{};
bool wasVHeld{}; // Last frame: the panel's hold on the game without rules (holdWithoutRules)
Tab tab{};
std::size_t chosenMark{};
View view{};
RuleView ruleView{};
bool isConfirmingDiscard{}; // The head asks before every unsaved change goes
bool showsMore{};
bool shouldScrollToChosen{};
float tuneHeight{};
std::array<char, 96> labelText{};
std::optional<std::uint32_t> labelFor;
std::optional<std::array<float, 3>> lampLabelFor;
std::uint64_t seenRestores{}; // The undos and redos the label field has reloaded after
bool isCompareDrawn{};

float px(float value)
{
    return std::round(value * ImGui::GetFontSize() / FontSize);
}

ImU32 swatchOf(const std::array<float, 3>& color)
{
    const auto channel = [](float value) { return static_cast<ImU32>(std::clamp(value, 0.0f, 1.0f) * 255); };

    return IM_COL32(channel(color[0]), channel(color[1]), channel(color[2]), 255);
}

ImU32 faded(ImU32 color)
{
    return ImGui::GetColorU32(color);
}

void text(ImVec2 at, ImU32 color, std::string_view line)
{
    ImGui::GetWindowDrawList()->AddText(at, faded(color), line.data(), line.data() + line.size());
}

float widthOf(std::string_view line)
{
    return ImGui::CalcTextSize(line.data(), line.data() + line.size()).x;
}

// Text cut off at `right`, so a long name leaves the note beside it readable.
void clippedText(ImVec2 at, float right, ImU32 color, std::string_view line)
{
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect({at.x, at.y}, {right, at.y + ImGui::GetTextLineHeight()}, true);
    text(at, color, line);
    draw->PopClipRect();
}

// A light's strength: whole above 10, two digits below, so a weak lamp does not read as 0.
std::string strengthText(float value)
{
    return value >= 10 ? std::format("{:.0f}", value) : std::format("{:.2g}", value);
}

void hairline(float y, ImU32 color = colour::Line)
{
    const float left = ImGui::GetWindowPos().x;
    ImGui::GetWindowDrawList()->AddLine({left, y}, {left + ImGui::GetWindowWidth(), y}, color);
}

void pushStyle()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {px(8), px(3)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {px(8), 0});
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, px(8));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Ground);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0u);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colour::Popup);
    ImGui::PushStyleColor(ImGuiCol_Border, colour::LineStrong);
    ImGui::PushStyleColor(ImGuiCol_Text, colour::Text);
    ImGui::PushStyleColor(ImGuiCol_Button, colour::Frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colour::FrameHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colour::FrameActive);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, colour::Frame);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colour::FrameHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colour::FrameActive);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, 0u);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, colour::FrameActive);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, colour::TextFaint);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, colour::TextDim);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(0xe6, 0xe4, 0xde, 0x48));
}

constexpr int StyleVars = 10, StyleColours = 16;

// A word that switches a view, bright while its view shows.
bool switchWord(const char* word, bool isCurrent)
{
    const ImVec2 at = ImGui::GetCursorScreenPos(), size{widthOf(word), ImGui::GetTextLineHeight()};
    const bool isPressed = ImGui::InvisibleButton(word, size);
    text(at, isCurrent ? colour::Text : ImGui::IsItemHovered() ? colour::TextDim : colour::TextFaint, word);

    return isPressed;
}

// A word that does something.
bool actionWord(const char* word)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool isPressed = ImGui::InvisibleButton(word, {widthOf(word), ImGui::GetTextLineHeight()});
    text(at, ImGui::IsItemHovered() ? colour::Text : colour::TextDim, word);

    return isPressed;
}

bool filledButton(const char* label, bool isEnabled, ImU32 fill, ImU32 hover, ImU32 press)
{
    ImGui::BeginDisabled(!isEnabled);
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, press);
    ImGui::PushStyleColor(ImGuiCol_Text, colour::Ground);
    const bool isPressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();

    return isPressed;
}

bool primaryButton(const char* label, bool isEnabled)
{
    return filledButton(label, isEnabled, colour::Text, IM_COL32_WHITE, colour::TextDim);
}

bool dangerButton(const char* label)
{
    return filledButton(label, true, colour::Bad, IM_COL32(0xf5, 0x9c, 0x8e, 0xff), IM_COL32(0xc8, 0x66, 0x58, 0xff));
}

// A thin track, a grab, and a tick at the original: as fxui's sliders look. The wheel nudges it.
bool track(const char* id, float& value, float low, float high, float tick, bool isLogarithmic)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = px(18), grab = px(5), centre = at.y + height / 2;
    ImGui::InvisibleButton(id, {width, height});

    const auto toPosition = [&](float v) {
        const float t = isLogarithmic ? std::log(std::max(v, low) / low) / std::log(high / low) : (v - low) / (high - low);
        return std::clamp(t, 0.0f, 1.0f);
    };
    const auto fromPosition = [&](float t) { return isLogarithmic ? low * std::pow(high / low, t) : low + (high - low) * t; };

    bool isChanged{};
    if (ImGui::IsItemActive())
    {
        value = fromPosition(std::clamp((ImGui::GetIO().MousePos.x - at.x - grab / 2) / (width - grab), 0.0f, 1.0f));
        isChanged = true;
    }
    else if (const float wheel = ImGui::GetIO().MouseWheel; ImGui::IsItemHovered() && wheel != 0)
    {
        value = fromPosition(std::clamp(toPosition(value) + wheel / 100, 0.0f, 1.0f));
        isChanged = true;
    }

    auto* draw = ImGui::GetWindowDrawList();
    const float position = at.x + toPosition(value) * (width - grab);
    draw->AddRectFilled({at.x, centre - 1}, {at.x + width, centre + 1}, faded(colour::LineStrong));
    draw->AddRectFilled({at.x, centre - 1}, {position, centre + 1}, faded(colour::Text));
    const float tickAt = at.x + toPosition(tick) * (width - grab) + grab / 2;
    draw->AddLine({tickAt, centre - px(5)}, {tickAt, centre + px(5)}, faded(colour::TextFaint));
    draw->AddRectFilled({position, centre - px(7)}, {position + grab, centre + px(7)}, faded(ImGui::IsItemActive() ? IM_COL32_WHITE : colour::Text));

    return isChanged;
}

// A row of the tuning zone: its label, then the control in the column beside it.
void rowLabel(const char* label)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    text({at.x, at.y + px(4)}, colour::TextDim, label);
    ImGui::SetCursorScreenPos({at.x + px(LabelWidth + 10), at.y});
}

void drawParameter(const Material& material, std::uint64_t parameter)
{
    auto value = valueOf(material.packed, parameter);
    if (value.empty()) return;

    const auto original = originalOf(material, parameter, value);
    const auto name = parameterText(parameter);
    bool isChanged{};
    for (std::size_t component = 0; component < value.size(); ++component)
    {
        const float start = component < original.size() ? original[component] : value[component];
        const float size = std::abs(start);
        const bool isWide = size >= 10;
        const float low = isWide ? size * LeastShare : std::min(0.0f, start * 4), high = std::max(1.0f, size * 4);
        const auto label = value.size() == 1 ? name : std::format("{}.{}", name, "xyzw"[component]);

        ImGui::PushID(static_cast<int>(component));
        ImGui::PushID(label.c_str());
        text(ImGui::GetCursorScreenPos(), colour::TextFaint, std::format("{}  {:.4g}", label, value[component]));
        ImGui::Dummy({1, ImGui::GetTextLineHeight() + px(2)});
        isChanged |= track("##value", value[component], low, high, start, isWide);
        ImGui::PopID();
        ImGui::PopID();
    }

    if (isChanged) edit(material, parameter, value);
}

void drawBrightness(const Material& material)
{
    auto value = valueOf(material.packed, SelfColorIntensityParameter);
    if (value.empty()) return;

    const auto original = originalOf(material, SelfColorIntensityParameter, value);
    const float start = original[0], size = std::max(start, 1.0f);

    rowLabel("Brightness");
    const bool isChanged = track("##brightness", value[0], size * LeastShare, size * 4, size, true);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(2));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(LabelWidth + 10));

    const ImVec2 at = ImGui::GetCursorScreenPos();
    const auto of = std::format("{:.4g} of {:.6g}", value[0], start);
    text(at, colour::Text, isComparing() ? std::string("Showing the original") : std::format("{:.3g} x original", start > 0 ? value[0] / start : 1));
    text({at.x + ImGui::GetContentRegionAvail().x - widthOf(of), at.y}, colour::TextDim, of);
    ImGui::Dummy({1, ImGui::GetTextLineHeight() + px(10)});

    if (isChanged) edit(material, SelfColorIntensityParameter, value);
}

void drawColour(const Material& material)
{
    auto value = valueOf(material.packed, SelfColorParameter);
    if (value.size() < 3) return;

    const auto original = originalOf(material, SelfColorParameter, value);
    rowLabel("Colour");

    constexpr ImGuiColorEditFlags Flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip;
    const float size = ImGui::GetFrameHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::ColorEdit3("##colour", value.data(), Flags)) edit(material, SelfColorParameter, value);

    text({at.x + size + px(8), at.y + px(4)}, colour::TextDim, value == original ? "unchanged" : "changed");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(8));
}

// The button and the key both hold it.
void drawCompare(const Material& material)
{
    isCompareDrawn = true;

    const bool canCompare = hasRule(material);
    const bool isKeyHeld = ImGui::IsKeyDown(ImGuiKey_C) && !ImGui::GetIO().WantTextInput && !isMoving;

    rowLabel("Compare");
    ImGui::BeginDisabled(!canCompare);
    ImGui::Button("Hold for original");
    const bool isHeld = ImGui::IsItemActive() || isKeyHeld;
    ImGui::EndDisabled();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(10));

    holdOriginal(material, isHeld && canCompare);
}

void drawLabel(const Material& material)
{
    if (!hasRule(material))
    {
        labelFor.reset();
        return;
    }

    // The label field is a lamp's or a material's: whichever it was last for reloads it.
    lampLabelFor.reset();
    if (labelFor != material.index)
    {
        labelFor = material.index;
        labelText = {};
        labelOf(material).copy(labelText.data(), labelText.size() - 1);
    }

    rowLabel("Label");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputTextWithHint("##label", "Name this rule", labelText.data(), labelText.size())) setLabel(material, labelText.data());

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(10));
}

void drawMore(const Material& material)
{
    std::vector<std::uint64_t> others;
    for (const auto lowHalf : material.packed.parameters)
    {
        const auto parameter = parameterOfLowHalf(lowHalf);
        if (parameter != SelfColorIntensityParameter && parameter != SelfColorParameter && !valueOf(material.packed, parameter).empty()) others.push_back(parameter);
    }

    const bool hasBrightness = !valueOf(material.packed, SelfColorIntensityParameter).empty();
    const bool canHide = hasBrightness && !others.empty();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (canHide && actionWord(showsMore ? "Fewer parameters" : "More parameters")) showsMore = !showsMore;

    if (hasRule(material))
    {
        ImGui::SetCursorScreenPos({at.x + ImGui::GetContentRegionAvail().x - widthOf("Back to original"), at.y});
        if (actionWord("Back to original")) removeRuleOn(material);
    }

    ImGui::SetCursorScreenPos({at.x, at.y + ImGui::GetTextLineHeight()});
    if (canHide && !showsMore) return;

    ImGui::Dummy({1, px(4)});
    for (const auto parameter : others) drawParameter(material, parameter);
}

std::string lampPlace(const std::array<float, 3>& at)
{
    return std::format("{:.1f}, {:.1f}, {:.1f}", at[0], at[1], at[2]);
}

void drawLampTune(const LampRow& lamp)
{
    text(ImGui::GetCursorScreenPos(), colour::TextDim, std::format("Lamp at {}{}", lampPlace(lamp.at), lamp.isDrawn ? "" : ", not drawn now"));
    ImGui::Dummy({1, ImGui::GetTextLineHeight() + px(8)});

    ImGui::BeginDisabled(isComparing());
    labelFor.reset();
    if (!lamp.isTuned) lampLabelFor.reset();
    else
    {
        if (lampLabelFor != lamp.at)
        {
            lampLabelFor = lamp.at;
            labelText = {};
            lamp.label.copy(labelText.data(), labelText.size() - 1);
        }

        rowLabel("Label");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputTextWithHint("##lampLabel", "Name this rule", labelText.data(), labelText.size())) setLampLabel(lamp, labelText.data());

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(10));
    }

    // Its light times this: the colour the game gives it stays.
    float brightness = lamp.brightness;
    rowLabel("Brightness");
    const bool isChanged = track("##lampBrightness", brightness, 0.001f, 8, 1, true);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(2));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(LabelWidth + 10));
    text(ImGui::GetCursorScreenPos(), colour::Text, isComparing() ? std::string("Showing the original") : std::format("{:.3g} x original", brightness));
    ImGui::Dummy({1, ImGui::GetTextLineHeight() + px(10)});
    if (isChanged) editLamp(lamp, brightness);

    ImGui::EndDisabled();

    isCompareDrawn = true;
    const bool isKeyHeld = ImGui::IsKeyDown(ImGuiKey_C) && !ImGui::GetIO().WantTextInput && !isMoving;
    rowLabel("Compare");
    ImGui::BeginDisabled(!lamp.isTuned);
    ImGui::Button("Hold for original");
    const bool isHeld = ImGui::IsItemActive() || isKeyHeld;
    ImGui::EndDisabled();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(10));
    holdOriginalLamp(lamp, isHeld && lamp.isTuned);

    if (!lamp.isTuned) return;

    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({at.x + ImGui::GetContentRegionAvail().x - widthOf("Back to original"), at.y});
    ImGui::BeginDisabled(isComparing());
    if (actionWord("Back to original")) resetLamp(lamp);
    ImGui::EndDisabled();
    ImGui::SetCursorScreenPos({at.x, at.y + ImGui::GetTextLineHeight()});
}

void drawTune()
{
    if (const auto lamp = selectedLamp())
    {
        drawLampTune(*lamp);
        return;
    }

    const auto material = selectedMaterial();
    if (!material)
    {
        text(ImGui::GetCursorScreenPos(), colour::TextDim, "Pick a material or a lamp. Hover one to see it flash.");
        ImGui::Dummy({1, ImGui::GetTextLineHeight()});
        return;
    }

    text(ImGui::GetCursorScreenPos(), colour::TextDim, std::format("{}, used {} times", hashText(material->name), material->uses));
    ImGui::Dummy({1, ImGui::GetTextLineHeight() + px(8)});

    ImGui::BeginDisabled(isComparing());
    drawLabel(*material);
    drawBrightness(*material);
    drawColour(*material);
    ImGui::EndDisabled();
    drawCompare(*material);
    ImGui::BeginDisabled(isComparing());
    drawMore(*material);
    ImGui::EndDisabled();
}

bool isChosen(const Pick& pick)
{
    if (pick.lamp)
    {
        const auto lamp = selectedLamp();
        return lamp && lamp->at == pick.lamp->at;
    }

    const auto material = selectedMaterial();
    return material && pick.material->poolRow == material->index;
}

void choose(const Pick& pick)
{
    if (pick.lamp) selectLamp(*pick.lamp);
    else if (pick.material->poolRow) select(*pick.material->poolRow, pick.material->name);

    shouldScrollToChosen = true;
}

bool canTakeKeys()
{
    return !isMoving && !ImGui::GetIO().WantTextInput && !isComparing();
}

// Ctrl+Z and Ctrl+Y (or Ctrl+Shift+Z), over either tab.
void undoKeys()
{
    const auto& io = ImGui::GetIO();
    if (!canTakeKeys() || !io.KeyCtrl) return;

    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? redo() : undo();
    else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
}

// After an undo or redo, the keys' or automation's, the label field reloads from the restored rule, or what was typed
// would go back in.
void reloadLabelsAfterRestore()
{
    if (std::exchange(seenRestores, restores()) == restores()) return;

    labelFor.reset();
    lampLabelFor.reset();
}

// The arrows walk the list; [ and ] step the chosen light's brightness, 0 puts it out and R takes its rule away.
void pickKeys(const std::vector<Pick>& picks)
{
    if (!canTakeKeys() || ImGui::GetIO().KeyCtrl || picks.empty()) return;

    const auto chosen = std::ranges::find_if(picks, isChosen);
    const auto at = static_cast<std::ptrdiff_t>(chosen - picks.begin());
    const bool isPicked = chosen != picks.end();
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) choose(picks[isPicked ? std::min<std::size_t>(at + 1, picks.size() - 1) : 0]);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) choose(picks[isPicked ? std::max<std::ptrdiff_t>(at - 1, 0) : 0]);
    if (!isPicked) return;

    const float step = ImGui::IsKeyPressed(ImGuiKey_RightBracket) ? BrightnessStep : ImGui::IsKeyPressed(ImGuiKey_LeftBracket) ? 1 / BrightnessStep : 0;
    const bool isOff = ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false);
    const bool isReset = ImGui::IsKeyPressed(ImGuiKey_R, false);
    if (const auto lamp = selectedLamp())
    {
        if (isReset) resetLamp(*lamp);
        else if (isOff) editLamp(*lamp, 0);
        else if (step) editLamp(*lamp, lamp->brightness > 0 ? lamp->brightness * step : FromOff);

        return;
    }

    const auto material = selectedMaterial();
    if (!material) return;

    if (isReset)
    {
        removeRuleOn(*material);
        return;
    }

    auto value = valueOf(material->packed, SelfColorIntensityParameter);
    if (value.empty() || (!step && !isOff)) return;

    const float original = originalOf(*material, SelfColorIntensityParameter, value)[0];
    value[0] = isOff ? 0 : value[0] > 0 ? value[0] * step : original * FromOff;
    edit(*material, SelfColorIntensityParameter, value);
}

// What a row of a list shows: a swatch and strength at the left, then the name, a note and a word that does
// something at the right.
struct RowLook
{
    std::optional<ImU32> swatch;
    std::string strength;
    ImU32 ink{colour::Text};
    std::string name;
    ImU32 nameInk{colour::Text};
    std::string note;
    ImU32 noteInk{colour::TextFaint};
    const char* action{};
};

struct RowHits
{
    bool isPressed{}, isHovered{}, isActionPressed{};
};

RowHits listRow(const RowLook& look, bool isChosen, bool canPick)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = px(26), left = at.x + px(Margin), line = at.y + (height - ImGui::GetTextLineHeight()) / 2;
    const float right = at.x + width - px(Margin), actionAt = look.action ? right - widthOf(look.action) : right;
    const float noteAt = (look.action ? actionAt - px(Margin) : right) - widthOf(look.note);

    // The row stops short of its action word, so a click on the word is the word's alone.
    RowHits hits;
    ImGui::BeginDisabled(!canPick);
    hits.isPressed = ImGui::InvisibleButton("##row", {look.action ? actionAt - px(Margin / 2) - at.x : width, height});
    hits.isHovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();

    auto* draw = ImGui::GetWindowDrawList();
    if (isChosen || hits.isHovered) draw->AddRectFilled(at, {at.x + width, at.y + height}, isChosen ? colour::Frame : colour::Raise);

    if (look.swatch)
    {
        draw->AddRectFilled({left, line + px(1)}, {left + px(12), line + px(13)}, *look.swatch);
        text({left + px(16), line}, look.ink, look.strength);
    }

    clippedText({look.swatch ? left + px(78) : left, line}, noteAt - px(8), look.nameInk, look.name);
    text({noteAt, line}, look.noteInk, look.note);

    if (look.action)
    {
        ImGui::SetCursorScreenPos({actionAt, line});
        ImGui::BeginDisabled(isComparing());
        hits.isActionPressed = actionWord(look.action);
        ImGui::EndDisabled();
    }

    hairline(at.y + height - 1);
    ImGui::SetCursorScreenPos({at.x, at.y + height});
    ImGui::Dummy({1, 0});

    return hits;
}

RowLook lookOf(const MaterialRow& row)
{
    const ImU32 ink = row.poolRow ? colour::Text : colour::TextFaint;
    RowLook look{.ink = ink,
        .name = row.label.empty() ? hashText(row.name) : row.label,
        .nameInk = row.label.empty() ? (row.poolRow ? colour::TextDim : colour::TextFaint) : ink,
        .noteInk = row.isTuned ? colour::Warn : colour::TextFaint};

    if (row.kind == MaterialKind::SelfLit)
    {
        look.swatch = row.poolRow ? swatchOf(row.color) : colour::LineStrong;
        look.strength = strengthText(row.brightness);
    }

    return look;
}

// Its colour as a swatch, scaled to its brightest channel, and that channel's value: the game's light at a metre.
RowLook lookOf(const LampRow& row)
{
    const float strongest = std::max(std::ranges::max(row.color), 1e-6f);
    const ImU32 ink = row.isDrawn ? colour::Text : colour::TextFaint;

    return {.swatch = swatchOf({row.color[0] / strongest, row.color[1] / strongest, row.color[2] / strongest}),
        .strength = strengthText(strongest),
        .ink = ink,
        .name = row.label.empty() ? lampPlace(row.at) : row.label,
        .nameInk = row.label.empty() ? colour::TextDim : ink,
        .note = row.isTuned ? std::format("{:.2g} x", row.brightness) : row.isDrawn ? std::string() : std::string("not drawn now"),
        .noteInk = row.isTuned ? colour::Warn : colour::TextFaint};
}

// A material's row: hovering flashes it in the game, a click picks it.
RowHits materialRow(const MaterialRow& row, const RowLook& look, bool isChosen)
{
    ImGui::PushID(static_cast<int>(row.poolRow.value_or(0)) ^ static_cast<int>(row.name));
    const auto hits = listRow(look, isChosen, row.poolRow.has_value());
    ImGui::PopID();

    if (hits.isHovered && row.poolRow) flash(*row.poolRow);
    if (hits.isPressed && row.poolRow) select(*row.poolRow, row.name);

    return hits;
}

RowHits lampRow(const LampRow& row, const RowLook& look, bool isChosen, bool canPick = true)
{
    ImGui::PushID(static_cast<int>(std::lround(row.at[0] * 100) ^ std::lround(row.at[1] * 1000) ^ std::lround(row.at[2] * 10000)));
    const auto hits = listRow(look, isChosen, canPick);
    ImGui::PopID();

    if (hits.isHovered) flashLamp(row.at);
    if (hits.isPressed) selectLamp(row);

    return hits;
}

// A mark's two lines, and its buttons at the right.
void markRow(std::size_t index, const Mark& mark, bool isChosen)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = px(48), left = at.x + px(Margin);
    const bool canGo = canGoTo(mark) && !isGoing();
    const float buttons = px(Margin + 72) + (isChosen ? px(48) : 0);

    ImGui::PushID(static_cast<int>(index));
    if (ImGui::InvisibleButton("##mark", {width - buttons, height}) && index != chosenMark)
    {
        chosenMark = index;
        clearSelection();
    }

    auto* draw = ImGui::GetWindowDrawList();
    const bool isHovered = ImGui::IsItemHovered();
    if (isChosen || isHovered) draw->AddRectFilled(at, {at.x + width, at.y + height}, isChosen ? colour::Raise : colour::Frame);
    if (isChosen) draw->AddRectFilled(at, {at.x + px(2), at.y + height}, colour::Text);

    text({left, at.y + px(7)}, colour::TextDim, std::to_string(index + 1));
    text({left + px(26), at.y + px(7)}, colour::Text, placeText(mark));
    const auto tuned = tunedCount(mark);
    text({left + px(26), at.y + px(26)}, colour::TextFaint,
        std::format("{}, {} lamps, {} glows{}", timeText(mark.clock), mark.lamps.size(), countOf(mark, MaterialKind::SelfLit), tuned ? std::format(", {} tuned", tuned) : ""));

    ImGui::SetCursorScreenPos({at.x + width - px(Margin + 72), at.y + px(6)});
    ImGui::BeginDisabled(!canGo);
    if (ImGui::Button("Go there")) goTo(mark);
    ImGui::EndDisabled();

    if (isChosen)
    {
        ImGui::SetCursorScreenPos({at.x + width - px(Margin + 72 + 48), at.y + px(6)});
        if (ImGui::Button("Done"))
        {
            removeMark(index);
            chosenMark = chosenMark > 0 ? chosenMark - 1 : 0;
        }
    }

    ImGui::PopID();
    hairline(at.y + height - 1);
    ImGui::SetCursorScreenPos({at.x, at.y + height});
    ImGui::Dummy({1, 0});
}

float hintsHeight()
{
    return ImGui::GetTextLineHeight() * 3 + px(18);
}

void drawEmpty(std::string_view first, std::string_view second)
{
    ImGui::Dummy({1, px(10)});
    const ImVec2 at = ImGui::GetCursorScreenPos();
    text({at.x + px(Margin), at.y}, colour::Text, first);
    text({at.x + px(Margin), at.y + ImGui::GetTextLineHeight() + px(4)}, colour::TextDim, second);
}

// The line over a list: what it shows at the left, the words that switch it at the right. True when one did.
template<class V> bool viewLine(std::string_view caption, V& current, std::initializer_list<std::pair<const char*, V>> words)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = px(26), line = at.y + (height - ImGui::GetTextLineHeight()) / 2;
    text({at.x + px(Margin), line}, colour::TextFaint, caption);

    bool isSwitched{};
    float x = at.x + ImGui::GetContentRegionAvail().x - px(Margin);
    for (auto word = std::rbegin(words); word != std::rend(words); ++word)
    {
        x -= widthOf(word->first);
        ImGui::SetCursorScreenPos({x, line});
        if (switchWord(word->first, current == word->second) && current != word->second)
        {
            current = word->second;
            isSwitched = true;
        }

        x -= px(14);
    }

    ImGui::SetCursorScreenPos({at.x, at.y + height});
    hairline(at.y + height - 1);

    return isSwitched;
}

// A faint line naming the rows below it.
void sectionLine(const char* name)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    text({at.x + px(Margin), at.y + px(6)}, colour::TextFaint, name);
    ImGui::Dummy({1, ImGui::GetTextLineHeight() + px(8)});
}

void materialNote(const MaterialRow& row, bool isChosen)
{
    auto look = lookOf(row);
    look.note = row.isTuned ? std::string("tuned") : row.poolRow ? std::format("used {}", row.uses) : std::string("not loaded here");
    materialRow(row, look, isChosen);
}

void drawLights(const Mark& mark)
{
    const auto lamps = lampRowsOf(mark);
    const auto glows = rowsOf(mark, MaterialKind::SelfLit);
    std::vector<Pick> picks;
    for (const auto& lamp : lamps) picks.push_back({.lamp = lamp});
    for (const auto& glow : glows)
        if (glow.poolRow) picks.push_back({.material = glow});

    pickKeys(picks);

    if (lamps.empty() && glows.empty()) drawEmpty("No lights marked here.", "Lamps show with ray-traced lights or path tracing on.");

    const auto scrollToChosen = [](bool isChosenRow) {
        if (isChosenRow && std::exchange(shouldScrollToChosen, false)) ImGui::SetScrollHereY(0.5f);
    };

    if (!lamps.empty()) sectionLine("Lamps: the light on the room");
    for (const auto& lamp : lamps)
    {
        const bool isChosenRow = isChosen({.lamp = lamp});
        lampRow(lamp, lookOf(lamp), isChosenRow);
        scrollToChosen(isChosenRow);
    }

    if (!glows.empty()) sectionLine("Glows: how the light itself looks");
    for (const auto& glow : glows)
    {
        const bool isChosenRow = glow.poolRow && isChosen({.material = glow});
        materialNote(glow, isChosenRow);
        scrollToChosen(isChosenRow);
    }
}

void drawMaterials(const Mark& mark)
{
    if (viewLine(std::format("Seen at mark {}", chosenMark + 1), view, {{"Lights", View::Lights}, {"Glass", View::Glass}, {"All", View::All}})) clearSelection();

    if (ImGui::BeginChild("##list", {0, ImGui::GetContentRegionAvail().y - tuneHeight - hintsHeight()}))
    {
        if (view == View::Lights) drawLights(mark);
        else
        {
            const auto rows = rowsOf(mark, view == View::Glass ? std::optional(MaterialKind::Glass) : std::nullopt);
            if (rows.empty()) drawEmpty("None of this kind here.", "Try another view above.");

            std::vector<Pick> picks;
            for (const auto& row : rows)
                if (row.poolRow) picks.push_back({.material = row});

            pickKeys(picks);
            for (const auto& row : rows)
            {
                const bool isChosenRow = row.poolRow && isChosen({.material = row});
                materialNote(row, isChosenRow);
                if (isChosenRow && std::exchange(shouldScrollToChosen, false)) ImGui::SetScrollHereY(0.5f);
            }
        }
    }

    ImGui::EndChild();
}

bool drawMarks()
{
    const auto& list = marks();
    if (list.empty())
    {
        drawEmpty("No marks yet.", "Press F6 where something looks wrong.");
        return false;
    }

    chosenMark = std::min(chosenMark, list.size() - 1);
    const float rowHeight = px(48), shown = std::min(static_cast<float>(list.size()), MostMarkRows) * rowHeight;
    if (ImGui::BeginChild("##marks", {0, shown}))
        for (std::size_t i = 0; i < list.size(); ++i) markRow(i, list[i], i == chosenMark);

    ImGui::EndChild();
    if (chosenMark >= list.size()) return false;

    drawMaterials(list[chosenMark]);

    return true;
}

bool isShown(RuleState state)
{
    return ruleView == RuleView::All || (state == RuleState::Saved) == (ruleView == RuleView::Saved);
}

// An unsaved rule says how it differs from the file, in the colour of unsaved work.
std::string_view stateText(RuleState state)
{
    switch (state)
    {
    case RuleState::New: return "new";
    case RuleState::Changed: return "changed";
    case RuleState::Removed: return "removed";
    case RuleState::Saved: break;
    }

    return "";
}

void drawRuleCaption(std::size_t saved, std::size_t unsaved)
{
    const auto caption = ruleView == RuleView::Unsaved ? std::format("{} unsaved", unsaved)
        : ruleView == RuleView::Saved                  ? std::format("{} saved", saved)
                                                       : std::format("{} rules", saved + unsaved);

    viewLine(caption, ruleView, {{"Unsaved", RuleView::Unsaved}, {"Saved", RuleView::Saved}, {"All", RuleView::All}});
}

void drawRulesEmpty(bool hasAny)
{
    if (!hasAny) drawEmpty("No rules yet.", "Tune a light and it shows here.");
    else if (ruleView == RuleView::Unsaved) drawEmpty("Nothing unsaved.", "The file has every rule as it is now.");
    else drawEmpty("Nothing saved yet.", "Save puts the rules in the file.");
}

// A lamp has nothing to load: its rule shows wherever it is, to tune where it is drawn.
void drawLampRules(const std::vector<LampRuleRow>& rows)
{
    const auto chosen = selectedLamp();
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto& [rule, saved, state] = rows[i];
        if (!isShown(state)) continue;

        const LampRow lamp{.location = rule.location, .at = rule.at, .brightness = rule.brightness, .label = rule.label, .isTuned = true, .isDrawn = true};
        const bool isRemoved = state == RuleState::Removed, isSaved = state == RuleState::Saved;
        const RowLook look{.name = rule.label.empty() ? lampPlace(rule.at) : rule.label,
            .nameInk = isRemoved ? colour::TextFaint : colour::Text,
            .note = isSaved ? std::format("{:.2g} x", rule.brightness) : std::string(stateText(state)),
            .noteInk = isSaved ? colour::TextFaint : colour::Warn,
            .action = isSaved ? "Remove" : "Revert"};

        ImGui::PushID(static_cast<int>(i));
        const auto hits = lampRow(lamp, look, chosen && chosen->location == rule.location && chosen->at == rule.at, !isRemoved);
        ImGui::PopID();

        if (!hits.isActionPressed) continue;
        if (isSaved) removeLampRule(rule);
        else revertLampRule(rows[i]);
    }
}

void drawMaterialRules(const std::vector<RuleRow>& rows)
{
    const auto selected = selectedMaterial();
    for (const auto& row : rows)
    {
        if (!isShown(row.state)) continue;

        const auto intensity = std::ranges::find(row.rule.changes, SelfColorIntensityParameter, &Change::parameter);
        const auto& change = intensity != row.rule.changes.end() ? *intensity : row.rule.changes.front();
        const bool isSaved = row.state == RuleState::Saved;

        auto look = lookOf(row.material);
        look.note = !isSaved ? std::string(stateText(row.state)) : row.material.poolRow ? std::format("from {:.4g}", change.from[0]) : std::string();
        look.noteInk = isSaved ? colour::TextFaint : colour::Warn;
        look.action = isSaved ? "Remove" : "Revert";

        const auto hits = materialRow(row.material, look, selected && row.material.poolRow == selected->index);
        if (!hits.isActionPressed) continue;
        if (isSaved) removeRule(row.rule);
        else revertRule(row);
    }
}

bool drawRules()
{
    const auto rows = ruleRows();
    const auto lamps = lampRuleRows();
    const auto savedOf = [](const auto& list) { return static_cast<std::size_t>(std::ranges::count(list, RuleState::Saved, [](const auto& row) { return row.state; })); };
    const auto shownOf = [](const auto& list) { return std::ranges::any_of(list, [](const auto& row) { return isShown(row.state); }); };
    const std::size_t saved = savedOf(rows) + savedOf(lamps), unsaved = rows.size() + lamps.size() - saved;

    drawRuleCaption(saved, unsaved);

    const bool hasLamps = shownOf(lamps), hasMaterials = shownOf(rows);
    if (!hasLamps && !hasMaterials)
    {
        drawRulesEmpty(saved + unsaved != 0);
        return false;
    }

    if (ImGui::BeginChild("##rules", {0, ImGui::GetContentRegionAvail().y - tuneHeight - hintsHeight()}))
    {
        if (hasLamps) sectionLine("Lamps");
        drawLampRules(lamps);

        if (hasMaterials) sectionLine("Materials");
        drawMaterialRules(rows);
    }

    ImGui::EndChild();

    return true;
}

void drawHead()
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = px(36), line = at.y + (height - ImGui::GetTextLineHeight()) / 2, right = at.x + ImGui::GetContentRegionAvail().x - px(Margin);

    ImGui::SetCursorScreenPos({at.x + px(Margin), line});
    if (switchWord("Marks", tab == Tab::Marks)) tab = Tab::Marks;

    ImGui::SetCursorScreenPos({at.x + px(Margin + 56), line});
    if (switchWord("Rules", tab == Tab::Rules)) tab = Tab::Rules;

    const auto unsaved = unsavedRules();
    if (!unsaved || isComparing()) isConfirmingDiscard = false;

    const float buttonY = at.y + (height - ImGui::GetFrameHeight()) / 2, gap = px(10);
    float x = right;
    if (isConfirmingDiscard)
    {
        // The question stands alone in the head: the answer is one of its two words.
        x -= widthOf("Discard") + px(16);
        ImGui::SetCursorScreenPos({x, buttonY});
        if (dangerButton("Discard"))
        {
            discardUnsaved();
            isConfirmingDiscard = false;
        }

        x -= gap + widthOf("Keep");
        ImGui::SetCursorScreenPos({x, line});
        if (actionWord("Keep")) isConfirmingDiscard = false;

        const auto question = std::format("Discard {} unsaved?", unsaved);
        text({x - gap - widthOf(question), line}, colour::Warn, question);
    }
    else
    {
        x -= widthOf("Save") + px(16);
        ImGui::SetCursorScreenPos({x, buttonY});
        if (primaryButton("Save", unsaved != 0)) saveRules();

        if (unsaved)
        {
            x -= gap + widthOf("Discard");
            ImGui::SetCursorScreenPos({x, line});
            ImGui::BeginDisabled(isComparing());
            if (actionWord("Discard")) isConfirmingDiscard = true;

            ImGui::EndDisabled();
        }

        if (areRulesHeldOff())
        {
            constexpr std::string_view Without = "Without any rule";
            text({x - gap - widthOf(Without), line}, colour::Warn, Without);
        }
        else if (unsaved)
        {
            // A default name says only where and what: name them before they go in the file.
            const auto unnamed = defaultNamedRules();
            const auto state = didSaveFail() ? std::string("Save failed")
                                             : std::format("{} unsaved{}", unsaved, unnamed ? std::format(", {} not named", unnamed) : std::string());
            text({x - gap - widthOf(state), line}, didSaveFail() ? colour::Bad : colour::Warn, state);
        }
    }

    hairline(at.y + height - 1);
    ImGui::SetCursorScreenPos({at.x, at.y + height});
}

// Fixed at the foot, as tall as it was last frame.
void drawTuning(float windowBottom)
{
    const float top = windowBottom - tuneHeight - hintsHeight();
    const float most = ImGui::GetWindowHeight() * MostTuneShare;

    hairline(top, colour::LineStrong);
    ImGui::SetCursorScreenPos({ImGui::GetWindowPos().x + px(Margin), top + px(10)});

    const float before = ImGui::GetCursorPosY();
    ImGui::SetNextWindowSizeConstraints({0, 0}, {FLT_MAX, most - px(20)});
    if (ImGui::BeginChild("##tune", {ImGui::GetWindowWidth() - px(Margin * 2), 0}, ImGuiChildFlags_AutoResizeY)) drawTune();

    ImGui::EndChild();
    tuneHeight = ImGui::GetCursorPosY() - before + px(20);
}

void drawHints(float windowBottom)
{
    const float top = windowBottom - hintsHeight();

    hairline(top);
    const float left = ImGui::GetWindowPos().x + px(Margin);
    const float line = ImGui::GetTextLineHeight() + px(4);
    text({left, top + px(5)}, colour::TextFaint, "Up/Down pick  [ ] brightness  0 off  R reset");
    text({left, top + px(5) + line}, colour::TextFaint, "Ctrl+Z undo  C original  V without any rule");
    text({left, top + px(5) + line * 2}, colour::TextFaint, "Right mouse moves  F6 marks  F7 closes");
}

void drawPanel()
{
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = !isMoving;
    if (isMoving) ImGui::SetWindowFocus(nullptr);

    const float width = px(PanelWidth);
    ImGui::SetNextWindowPos({io.DisplaySize.x - width, 0});
    ImGui::SetNextWindowSize({width, io.DisplaySize.y});
    pushStyle();

    isCompareDrawn = false;

    constexpr ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##remasterPanel", nullptr, Flags | (isMoving ? ImGuiWindowFlags_NoInputs : 0)))
    {
        const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
        ImGui::GetWindowDrawList()->AddLine(ImGui::GetWindowPos(), {ImGui::GetWindowPos().x, bottom}, colour::LineStrong);

        // Held, the game shows as it would with no rule at all.
        // The panel holds for V alone; a hold from automation's `without` is its own and stays.
        const bool isVHeld = ImGui::IsKeyDown(ImGuiKey_V) && !ImGui::GetIO().WantTextInput && !isMoving && areRulesOn();
        if (isVHeld != wasVHeld) holdWithoutRules(Holder::Panel, isVHeld);

        wasVHeld = isVHeld;

        drawHead();
        undoKeys();
        reloadLabelsAfterRestore();
        if (!areRulesOn()) drawEmpty("The rules are off.", "F1 turns the enhancements back on.");
        else if (tab == Tab::Marks ? drawMarks() : drawRules()) drawTuning(bottom);

        drawHints(bottom);
    }

    ImGui::End();
    ImGui::PopStyleColor(StyleColours);
    ImGui::PopStyleVar(StyleVars);

    // Only the compare's own control lets it go, so without that control nothing else would. V lets itself go.
    if (!isCompareDrawn && !areRulesHeldOff() && isComparing()) settleTool();
}

void setOpen(bool open)
{
    isOpen = open;
    if (open) return;

    isBlocking = false;
    isConfirmingDiscard = false;
    settleTool();

    // Its own hold goes with it; one automation started stays until `without 0`.
    if (std::exchange(wasVHeld, false)) holdWithoutRules(Holder::Panel, false);
}

bool isAnyInputDown(reshade::api::effect_runtime* runtime)
{
    for (std::uint32_t key = FirstKey; key <= LastKey; ++key)
        if (runtime->is_key_down(key)) return true;

    for (std::uint32_t button = 0; button < MouseButtons; ++button)
        if (runtime->is_mouse_button_down(button)) return true;

    return false;
}

void onOverlay(reshade::api::effect_runtime*)
{
    if (!isOpen)
    {
        if (std::exchange(hasDrawnCursor, false)) ImGui::GetIO().MouseDrawCursor = false;
        return;
    }

    hasDrawnCursor = true;
    endFlash();
    refreshLoaded();
    drawPanel();
    startFlash();

    // ImGui has no current context at the present, so that keeps this from here.
    isTyping = ImGui::GetIO().WantTextInput;
}

void onPresent(reshade::api::effect_runtime* runtime)
{
    followTravel();

    if (runtime->is_key_pressed(VK_F6)) addMark();
    if (runtime->is_key_pressed(VK_F7)) setOpen(!isOpen);
    if (!isOpen) return;

    if (runtime->is_key_pressed(VK_ESCAPE) && !isTyping) setOpen(false);

    // While the right button is down the game has its input back, and the panel stays. A block swallows
    // releases too, so it starts once nothing is held: Snake must not walk on with a key the game saw go down.
    isMoving = runtime->is_mouse_button_down(RightMouse);
    isBlocking = !isMoving && (isBlocking || !isAnyInputDown(runtime));
    if (isBlocking) runtime->block_input_next_frame();
}
}

void showLamp(std::size_t mark, const LampRow& lamp)
{
    setOpen(true);
    tab = Tab::Marks;
    chosenMark = mark;
    view = View::Lights;
    selectLamp(lamp);
}

void registerPanel() noexcept
{
    reshade::register_event<reshade::addon_event::reshade_overlay>(onOverlay);
    reshade::register_event<reshade::addon_event::reshade_present>(onPresent);
}
}
