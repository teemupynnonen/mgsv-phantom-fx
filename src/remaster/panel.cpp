// The remastering tool's panel: over the game at the screen's right edge, on its own keys, without ReShade's
// window. Only a dev build has this file (remaster.cmake).
#include "fxui/fxui.hpp"
#include "remaster/game_materials.hpp"
#include "remaster/in_force.hpp"
#include "remaster/tool.hpp"

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
constexpr std::uint32_t FirstKey = 0x08, LastKey = 0xFE;

enum class Tab : std::uint8_t
{
    Marks,
    Rules
};

bool isOpen{}, isMoving{}, isTyping{}, isBlocking{}, hasDrawnCursor{};
Tab tab{};
std::size_t chosenMark{};
std::optional<MaterialKind> shownKind{MaterialKind::SelfLit};
bool showsMore{};
float tuneHeight{};
std::array<char, 96> labelText{};
std::optional<std::uint32_t> labelFor;
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

bool primaryButton(const char* label, bool isEnabled)
{
    ImGui::BeginDisabled(!isEnabled);
    ImGui::PushStyleColor(ImGuiCol_Button, colour::Text);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32_WHITE);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colour::TextDim);
    ImGui::PushStyleColor(ImGuiCol_Text, colour::Ground);
    const bool isPressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();

    return isPressed;
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
        const float low = isWide ? size / 1000 : std::min(0.0f, start * 4), high = std::max(1.0f, size * 4);
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
    const bool isChanged = track("##brightness", value[0], size / 1000, size * 4, size, true);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(2));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(LabelWidth + 10));

    const ImVec2 at = ImGui::GetCursorScreenPos();
    const auto of = std::format("{:.0f} of {:.0f}", value[0], start);
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

void drawTune()
{
    const auto material = selectedMaterial();
    if (!material)
    {
        text(ImGui::GetCursorScreenPos(), colour::TextDim, "Pick a material. Hover one to see it flash.");
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

void materialRow(const MaterialRow& row, bool isChosen, std::string_view note)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = px(26), left = at.x + px(Margin), line = at.y + (height - ImGui::GetTextLineHeight()) / 2;

    ImGui::PushID(static_cast<int>(row.poolRow.value_or(0)) ^ static_cast<int>(row.name));
    ImGui::BeginDisabled(!row.poolRow);
    const bool isPressed = ImGui::InvisibleButton("##row", {width, height});
    const bool isHovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    ImGui::PopID();

    auto* draw = ImGui::GetWindowDrawList();
    if (isChosen || isHovered) draw->AddRectFilled(at, {at.x + width, at.y + height}, isChosen ? colour::Frame : colour::Raise);

    const ImU32 ink = row.poolRow ? colour::Text : colour::TextFaint;
    const bool isLit = row.kind == MaterialKind::SelfLit;
    if (isLit)
    {
        draw->AddRectFilled({left, line + px(1)}, {left + px(12), line + px(13)}, row.poolRow ? swatchOf(row.color) : colour::LineStrong);
        text({left + px(16), line}, ink, std::format("{:.0f}", row.brightness));
    }

    const float nameAt = isLit ? left + px(78) : left;
    text({nameAt, line}, row.label.empty() ? (row.poolRow ? colour::TextDim : colour::TextFaint) : ink, row.label.empty() ? hashText(row.name) : row.label);
    text({at.x + width - px(Margin) - widthOf(note), line}, row.isTuned ? colour::Warn : colour::TextFaint, note);
    hairline(at.y + height - 1);

    if (isHovered && row.poolRow) flash(*row.poolRow);
    if (isPressed && row.poolRow) select(*row.poolRow, row.name);
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
    text({left + px(26), at.y + px(26)}, colour::TextFaint, std::format("{}, {} self-lit{}", timeText(mark.clock), countOf(mark, MaterialKind::SelfLit), tuned ? std::format(", {} tuned", tuned) : ""));

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
    return ImGui::GetTextLineHeight() + px(10);
}

void drawEmpty(std::string_view first, std::string_view second)
{
    ImGui::Dummy({1, px(10)});
    const ImVec2 at = ImGui::GetCursorScreenPos();
    text({at.x + px(Margin), at.y}, colour::Text, first);
    text({at.x + px(Margin), at.y + ImGui::GetTextLineHeight() + px(4)}, colour::TextDim, second);
}

void kindWord(const char* word, std::optional<MaterialKind> kind)
{
    if (!switchWord(word, shownKind == kind) || shownKind == kind) return;

    shownKind = kind;
    clearSelection();
}

void drawMaterials(const Mark& mark)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = px(26), line = at.y + (height - ImGui::GetTextLineHeight()) / 2, right = at.x + ImGui::GetContentRegionAvail().x - px(Margin);
    text({at.x + px(Margin), line}, colour::TextFaint, std::format("Seen at mark {}", chosenMark + 1));

    const float gap = px(14), all = right - widthOf("All"), glass = all - gap - widthOf("Glass"), selfLit = glass - gap - widthOf("Self-lit");
    ImGui::SetCursorScreenPos({selfLit, line});
    kindWord("Self-lit", MaterialKind::SelfLit);
    ImGui::SetCursorScreenPos({glass, line});
    kindWord("Glass", MaterialKind::Glass);
    ImGui::SetCursorScreenPos({all, line});
    kindWord("All", std::nullopt);

    ImGui::SetCursorScreenPos({at.x, at.y + height});
    hairline(at.y + height - 1);

    const auto rows = rowsOf(mark, shownKind);
    if (ImGui::BeginChild("##materials", {0, ImGui::GetContentRegionAvail().y - tuneHeight - hintsHeight()}))
    {
        if (rows.empty()) drawEmpty("None of this kind here.", "Try another kind above.");

        const auto selected = selectedMaterial();
        for (const auto& row : rows)
        {
            const auto note = row.isTuned ? std::string("tuned") : row.poolRow ? std::format("used {}", row.uses) : std::string("not loaded here");
            materialRow(row, selected && row.poolRow == selected->index, note);
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
    const float rowHeight = px(48), shown = std::min(static_cast<float>(list.size()) * rowHeight, ImGui::GetContentRegionAvail().y / 3);
    if (ImGui::BeginChild("##marks", {0, shown}))
        for (std::size_t i = 0; i < list.size(); ++i) markRow(i, list[i], i == chosenMark);

    ImGui::EndChild();
    if (chosenMark >= list.size()) return false;

    drawMaterials(list[chosenMark]);

    return true;
}

bool drawRules()
{
    const auto rows = ruleRows();
    if (rows.empty())
    {
        drawEmpty("No rules yet.", "Tune a material and it shows here.");
        return false;
    }

    const auto selected = selectedMaterial();
    if (ImGui::BeginChild("##rules", {0, ImGui::GetContentRegionAvail().y - tuneHeight - hintsHeight()}))
    {
        for (const auto& row : rows)
        {
            const auto intensity = std::ranges::find(row.rule.changes, SelfColorIntensityParameter, &Change::parameter);
            const auto& change = intensity != row.rule.changes.end() ? *intensity : row.rule.changes.front();
            const bool isLoaded = row.material.poolRow.has_value();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            materialRow(row.material, selected && row.material.poolRow == selected->index, isLoaded ? std::format("from {:.4g}", change.from[0]) : "");
            if (isLoaded) continue;

            // Elsewhere there is nothing to tune, but a rule can still go.
            const float width = widthOf("Remove") + px(16), next = ImGui::GetCursorPosY();
            ImGui::SetCursorScreenPos({at.x + ImGui::GetContentRegionAvail().x - px(Margin) - width, at.y + (px(26) - ImGui::GetFrameHeight()) / 2});
            ImGui::PushID(&row);
            if (ImGui::Button("Remove")) removeRule(row.rule);

            ImGui::PopID();
            ImGui::SetCursorPosY(next);
        }
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
    const float save = widthOf("Save") + px(16);
    if (unsaved)
    {
        const auto state = didSaveFail() ? std::string("Save failed") : std::format("{} unsaved", unsaved);
        text({right - save - px(10) - widthOf(state), line}, didSaveFail() ? colour::Bad : colour::Warn, state);
    }

    ImGui::SetCursorScreenPos({right - save, at.y + (height - ImGui::GetFrameHeight()) / 2});
    if (primaryButton("Save", unsaved != 0)) saveRules();

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
    text({ImGui::GetWindowPos().x + px(Margin), top + px(5)}, colour::TextFaint, "F6 marks   Right mouse moves   C original   F7 closes");
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

        drawHead();
        if (!areRulesOn()) drawEmpty("The rules are off.", "F1 turns the enhancements back on.");
        else if (tab == Tab::Marks ? drawMarks() : drawRules()) drawTuning(bottom);

        drawHints(bottom);
    }

    ImGui::End();
    ImGui::PopStyleColor(StyleColours);
    ImGui::PopStyleVar(StyleVars);

    // Only the compare's own control lets it go, so without that control nothing else would.
    if (!isCompareDrawn && isComparing()) settleTool();
}

void setOpen(bool open)
{
    isOpen = open;
    if (open) return;

    isBlocking = false;
    settleTool();
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

void registerPanel() noexcept
{
    reshade::register_event<reshade::addon_event::reshade_overlay>(onOverlay);
    reshade::register_event<reshade::addon_event::reshade_present>(onPresent);
}
}
