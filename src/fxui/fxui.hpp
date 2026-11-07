#pragma once

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// fxui: a settings window for ReShade add-ons, drawn with the Dear ImGui that ReShade gives add-ons.
// The window is a frame budget: every effect is a line with its value and its GPU milliseconds on one
// scale, the frame split by effect heads the window, and tabs under it each hold one or more sections
// of lines. A line folds out to its settings. README.md shows a whole window.
//
// Each frame, inside the overlay callback: fill a Budget, open a Window, then draw the sections:
//
//     fxui::Window window(sections, budget, chrome);
//     if (fxui::Page page(Section::Lighting); page)
//         if (fxui::Item item({.id = "softShadows", .name = "Soft shadows"}); item)
//             fxui::slider("Sun size", settings.sunSize, {.min = 0.1f, .max = 4, .standard = 0.53f});
//
// Every call that draws must include <reshade.hpp> after <imgui.h>, which routes ImGui through ReShade.
// One window at a time: it keeps its page, search and scroll target between frames.
namespace fxui
{
// The window's own palette.
namespace colour
{
inline constexpr ImU32 Ground = IM_COL32(0x14, 0x17, 0x1b, 0xff);
inline constexpr ImU32 Rail = IM_COL32(0x0f, 0x11, 0x14, 0xff);
inline constexpr ImU32 Raise = IM_COL32(0x1a, 0x1e, 0x23, 0xff);
inline constexpr ImU32 Popup = IM_COL32(0x1b, 0x1f, 0x25, 0xff);
inline constexpr ImU32 Frame = IM_COL32(0x22, 0x27, 0x2e, 0xff);
inline constexpr ImU32 FrameHover = IM_COL32(0x2b, 0x31, 0x3a, 0xff);
inline constexpr ImU32 FrameActive = IM_COL32(0x35, 0x3c, 0x46, 0xff);
inline constexpr ImU32 Line = IM_COL32(0x26, 0x2b, 0x32, 0xff);
inline constexpr ImU32 LineStrong = IM_COL32(0x39, 0x40, 0x4a, 0xff);
inline constexpr ImU32 Text = IM_COL32(0xe6, 0xe4, 0xde, 0xff);
inline constexpr ImU32 TextDim = IM_COL32(0x9c, 0xa1, 0xa7, 0xff);
inline constexpr ImU32 TextFaint = IM_COL32(0x68, 0x6e, 0x75, 0xff);
inline constexpr ImU32 Ok = IM_COL32(0x8f, 0xd1, 0x8e, 0xff);
inline constexpr ImU32 Warn = IM_COL32(0xef, 0xb3, 0x5c, 0xff);
inline constexpr ImU32 Bad = IM_COL32(0xef, 0x7d, 0x6d, 0xff);
// The host game's own share of the frame.
inline constexpr ImU32 Game = IM_COL32(0x3a, 0x40, 0x48, 0xff);
}

// Section hues: equally light on the ground, and apart from the state colours.
namespace hue
{
inline constexpr ImU32 Teal = IM_COL32(0x5e, 0xc4, 0xb6, 0xff);
inline constexpr ImU32 Straw = IM_COL32(0xd8, 0xcd, 0x78, 0xff);
inline constexpr ImU32 Blue = IM_COL32(0x7a, 0xa8, 0xee, 0xff);
inline constexpr ImU32 Violet = IM_COL32(0xa9, 0x9c, 0xe8, 0xff);
inline constexpr ImU32 Pink = IM_COL32(0xe0, 0x91, 0xb8, 0xff);
inline constexpr ImU32 Sand = IM_COL32(0xb8, 0xae, 0x9e, 0xff);
}

// ---- What the add-on reports each frame ----

enum class State : std::uint8_t
{
    Working,
    Off,
    Held,        // On, but held off by the window's master switch
    Replaced,    // On, but the line in `replacedBy` replaces it while that one is on
    Restart,     // Changed, and the change takes a restart of the game
    NotRunning,  // On, but not running; the message says why
    Unavailable, // Failed; the message says why
};

struct Pass
{
    std::string_view name;
    float ms{};
};

// An effect whose GPU work is timed: a row in the budget, a slice of the frame bar, and the state and
// cost of the line item with the same id.
struct Line
{
    std::string_view id;
    std::string_view name;
    std::string_view tech; // Its technical name, dim beside the name
    int section{};
    State state{State::Off};
    float ms{};               // Measured; 0 where it did not run
    std::vector<Pass> passes; // The budget's breakdown
    bool* toggle{};           // Its on/off setting, switched from the budget and from its item
    std::string_view replacedBy;
    std::string message;      // Why it is not running, or what a restart changes
};

struct Budget
{
    std::vector<Line> lines;
    float gpuMs{};   // The GPU's whole frame
    float frameMs{}; // Between the frames the game renders and presents
    unsigned presented{1}; // Frames shown per rendered frame: more with frame generation

    float total() const;
    float total(int section) const;
    const Line* find(std::string_view id) const;
};

// ---- The window's shape ----

struct Section
{
    const char* name{};
    ImU32 hue{};     // Its lines' colour; 0 for a section without timed lines
    bool overview{}; // The budget page: no items, so a search leaves it out
    // The tab it shows on. Sections with the same tab share it, each under its name; without one, a
    // section has a tab of its own.
    const char* tab{};
};

struct Chrome
{
    const char* name{"Effects"}; // The add-on's name, for its share of the frame
    // The master switch in the top strip, such as all visual effects on F1; none without `master`.
    bool* master{};
    const char* masterLabel{};
    const char* masterKey{};
    const char* masterTip{};
    const char* masterOff{}; // Beside the switch while it is off
};

// The strip, the frame bar and the tabs, then the page area until it goes out of scope. `sections`
// and `budget` must outlive it.
class Window
{
public:
    Window(std::span<const Section> sections, const Budget& budget, const Chrome& chrome = {});
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};

// A section: true while it shows, when its tab is chosen or a search runs. On a tab of several
// sections it starts with its name. During a search only items show, so its settings belong in items.
class Page
{
public:
    explicit Page(int section);
    template<class E> requires std::is_enum_v<E>
    explicit Page(E section) : Page(static_cast<int>(section)) {}
    ~Page();
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    explicit operator bool() const { return shown; }

private:
    bool shown{};
};

struct ItemSpec
{
    // Unique; a budget line with the same id gives the item its state and cost, and the name and tech
    // it leaves out.
    const char* id{};
    const char* name{};
    const char* tech{};     // Its technical name, dim beside the name
    const char* tag{};      // An outlined tag, such as "Experimental", before the summary
    const char* tip{};      // The long explanation, on the name's tooltip
    const char* summary{};  // One plain line, first when it folds out
    const char* keywords{}; // More words a search finds it by, such as its settings' labels
    bool isChanged{};       // A setting in it is not at its default: a dot after the name
    // Without a budget line of the same id:
    bool* toggle{};
    std::optional<State> state;
    std::string_view message;
    std::optional<float> ms; // A cost to show, such as the sum of the lines it sets
};

// A line item: its name, value, cost and a fold mark on one line. Clicked, it folds out to its
// summary, messages and settings, and the one open before folds back; during a search every match is
// open. True while its settings show: open, on (or without a switch) and not replaced. A switch is its
// value, On or Off; an item without one may give its line a value with itemChoice().
class Item
{
public:
    explicit Item(const ItemSpec& spec);
    ~Item();
    Item(const Item&) = delete;
    Item& operator=(const Item&) = delete;

    explicit operator bool() const { return open; }

private:
    const char* id{};
    bool drawn{}, isUnfolded{}, open{}, held{}, hot{};
    float top{}, left{}, right{};
};

// An item without settings of its own.
void item(const ItemSpec& spec);

// ---- Settings, in an item: a label, the control, its readout and a reset button ----

struct SliderSpec
{
    float min{}, max{1};
    std::optional<float> standard; // The default the reset button returns to
    const char* format{"%.2f"};    // printf format of the readout
    float shown{1};                // Multiplies the value in the readout, such as 100 for a percentage
    std::string (*text)(float value){}; // A readout that a format cannot write, such as a clock
    bool logarithmic{};
    bool onRelease{}; // Changes the value only when released, for values that are slow to apply
    const char* tip{};
};

// True when the value changed.
bool slider(const char* label, float& value, const SliderSpec& spec);

using Icon = void (*)(ImDrawList* draw, ImVec2 centre, float radius, ImU32 colour);

struct Option
{
    const char* label{};
    Icon icon{};
    bool enabled{true};
    const char* tip{};
};

template<class T>
struct Choice
{
    T value{};
    const char* label{};
    Icon icon{};
    bool enabled{true};
    const char* tip{};
};

// The value on the item's line: the option chosen, with arrows to step through the enabled ones, and,
// with more than two, a list on a click. Inside an Item, folded or open. Returns the index picked, or -1.
int itemChoice(int selected, std::span<const Option> options);

// Side-by-side buttons for a few choices, with a reset while `standard` (an index, or -1 for none) is
// not chosen. Returns the index pressed, or -1.
int segmented(const char* label, int selected, std::span<const Option> options, const char* tip = nullptr, int standard = -1);

// A combo box: true while open, then option() for each entry and endCombo(). `isChanged` shows a reset
// beside it, and `reset` says whether it was pressed.
bool beginCombo(const char* label, const char* preview, const char* tip = nullptr, bool isChanged = false, bool* reset = nullptr);
bool option(const char* label, bool selected, bool enabled = true, const char* tip = nullptr);
void endCombo();

// With a reset beside it while it differs from `standard`.
bool checkbox(const char* label, bool& value, const char* tip = nullptr, std::optional<bool> standard = {});
bool button(const char* label, const char* tip = nullptr, bool enabled = true);
// A box of text across the item, `lines` tall: text to edit, such as code, true when Ctrl+Enter
// submits it; or, read-only, text to select and copy, such as output.
bool textBox(const char* id, std::span<char> text, float lines, bool readOnly = false);
// Text under a control, in the control's column.
void hint(std::string_view text);

enum class Tone : std::uint8_t { Dim, Ok, Warn, Bad };

// A line of text at the current indent: grey, or in a state colour.
void note(Tone tone, std::string_view text);
// A page's one line on what it does now, with a dot in its tone. Hidden during a search.
void statusLine(Tone tone, std::string_view text);
// A label with a rule to its right, above a group of settings.
void subheading(const char* text);
// Vertical space, in pixels at the 13 px font.
void gap(float pixels);
// A label and its value, in the settings' columns.
void fact(const char* label, std::string_view value);
// Underlined text that jumps to the item `id`, and its page.
bool link(const char* label, std::string_view id);
// The item `id`: chooses its page, scrolls to it and flashes it.
void goTo(std::string_view id);
// The long explanation of the last control, while the mouse rests on it.
void tooltip(const char* text);
// A search runs: pages show their matching items, from every section.
bool isSearching();

// ---- The budget page ----

// Every line with its switch, state, cost and share of the frame; each opens to its passes, and its
// name jumps to its item. Then the total, the game's own work and the GPU frame. True when a switch
// changed.
bool ledger();
// A row of another table of costs, such as measured ones: a name, a bar and milliseconds.
void costRow(std::string_view name, float ms, ImU32 colour = colour::TextDim);

// The costs in a corner over the game, also with ReShade's overlay closed: for the add-on's
// reshade_overlay event. True while it shows, to add rows before it goes out of scope.
class CostOverlay
{
public:
    CostOverlay(std::span<const Section> sections, const Budget& budget, const char* name);
    ~CostOverlay();
    CostOverlay(const CostOverlay&) = delete;
    CostOverlay& operator=(const CostOverlay&) = delete;

    explicit operator bool() const { return open; }

private:
    bool open{};
    int colours{}, vars{};
};

// A short message over the game for a few seconds, such as "Frame captured", with a dot in its tone
// and a dim detail after it. A new message replaces the one showing. Any thread may call it.
void toast(Tone tone, std::string_view text, std::string_view detail = {});

// A side or corner of the screen.
enum class Placement : std::uint8_t { TopLeft, Top, TopRight, Left, Right, BottomLeft, Bottom, BottomRight };

// Draws the message at `placement`, over the game and the window, also with ReShade's overlay closed:
// for the add-on's reshade_overlay event.
void drawToast(Placement placement = Placement::Right);

// ---- Typed conveniences ----

template<class T>
bool segmented(const char* label, T& value, std::span<const Choice<T>> choices, const char* tip = nullptr, std::optional<T> standard = {})
{
    std::vector<Option> options;
    int selected = -1, standardIndex = -1;
    for (const auto& choice : choices)
    {
        if (choice.value == value) selected = static_cast<int>(options.size());
        if (standard && choice.value == *standard) standardIndex = static_cast<int>(options.size());
        options.push_back({choice.label, choice.icon, choice.enabled, choice.tip});
    }

    const int pressed = segmented(label, selected, options, tip, standardIndex);
    if (pressed < 0 || pressed == selected) return false;

    value = choices[static_cast<std::size_t>(pressed)].value;
    return true;
}

template<class T>
bool combo(const char* label, T& value, std::span<const Choice<T>> choices, const char* tip = nullptr, std::optional<T> standard = {})
{
    const char* preview = "";
    for (const auto& choice : choices)
        if (choice.value == value) preview = choice.label;

    bool reset{}, changed{};
    if (beginCombo(label, preview, tip, standard && value != *standard, &reset))
    {
        for (const auto& choice : choices)
            if (option(choice.label, choice.value == value, choice.enabled, choice.tip) && choice.value != value)
            {
                value = choice.value;
                changed = true;
            }

        endCombo();
    }

    if (!reset) return changed;

    value = *standard;
    return true;
}

template<class T, std::size_t N>
bool segmented(const char* label, T& value, const Choice<T> (&choices)[N], const char* tip = nullptr, std::optional<T> standard = {})
{
    return segmented(label, value, std::span<const Choice<T>>(choices), tip, standard);
}

template<class T, std::size_t N>
bool combo(const char* label, T& value, const Choice<T> (&choices)[N], const char* tip = nullptr, std::optional<T> standard = {})
{
    return combo(label, value, std::span<const Choice<T>>(choices), tip, standard);
}

// A combo whose list starts with a search box and shows the names that hold every word of it, in any
// case: for long lists. Returns the index of the name picked.
std::optional<std::size_t> searchCombo(const char* label, const char* preview, std::span<const std::string_view> names, std::optional<std::size_t> selected,
    const char* tip = nullptr);
}
