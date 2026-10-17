#pragma once
// =============================================================================
//  UI layout and interaction model (pure C++, no GPU, unit-tested).
//  Coordinates are DESIGN units = Main-Faceplate.png pixels (3840 wide). Everything
//  sits on a measured faceplate region (Faceplate.hpp):
//    top bar      : status lamp in the logo socket, engraved title
//    name panel   : device + format (engraved)
//    name panel   : device + format (click: next device, right-click: previous)
//    6 sockets    : LOAD stems, |◀ to start, PLAY / PAUSE, STOP, LOOP, SOURCE (tones / inputs / stems)
//    glass display: song position + length, DSP load, driver / buffer / latency / dropouts
//    4 tabs       : knob page MIX (trim, pan, aux sends) / EQ / DYNAMICS / FORGE (Beat Forge:
//                   strips 1–8 = the 8 tracks' sound knobs + LOCK, strips 9–12 = composer, space, sounds)
//    bus titles   : click MAIN / AUX 1–3 = what the 13 stem faders control (main mix or that aux send)
//    left panel   : 4 bus strips (Main, Aux 1–3)
//    main panel   : 13 stem strips
//    top bar      : title, OPEN / SAVE / EXPORT, status line (engraved)
//    bottom bar   : song overview (click / drag to seek), LIM (main limiter) + main output L/R meter
//  The left/main panels grow with Canvas::extraH (windows taller than 16:9).
// =============================================================================
#include "hybrid/core/Config.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/ui/Faceplate.hpp"
#include "hybrid/ui/Geometry.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hc::ui {

enum class WidgetKind : std::uint8_t { Knob, SendKnob, Fader, Button, StemMeter, BusMeter, MasterMeter, Transport, Tab, NamePanel, Overview,
                                       Command, BusTitle };
enum class UiAction : std::uint8_t { None, PrevDevice, NextDevice, LoadStems, CycleSource, ClearSolo, ClearMute, FaderBank,
                                     ToStart, PlayPause, Stop, ToggleLoop, Seek, SelectPage,
                                     OpenSession, SaveSession, ExportMix, ForgeGenerate, ForgeUndo, ForgeClear, ToggleScope };

// What the 5 knobs of every stem strip control.
enum class Page : std::uint8_t { Mix, Eq, Dynamics, Forge };
inline constexpr std::uint32_t kPages      = 4;
inline constexpr std::uint32_t kMixerPages = 3;   // pages whose knobs are the stem's own controls
inline constexpr std::uint32_t kStripKnobs = 5;   // 2 large + 3 small per stem strip
// Parameter field ("stemNN.<field>") and caption of each knob on each page.
inline constexpr const char* kPageFields[kMixerPages][kStripKnobs]   = {{"trim", "pan", "aux1", "aux2", "aux3"},
                                                                   {"lo", "hi", "hpf", "mid", "midf"},
                                                                   {"thr", "ratio", "att", "rel", "mkup"}};
inline constexpr const char* kPageCaptions[kMixerPages][kStripKnobs] = {{"TRIM", "PAN", "A1", "A2", "A3"},
                                                                   {"LOW", "HIGH", "CUT", "MID", "FREQ"},
                                                                   {"THRESH", "RATIO", "ATK", "REL", "GAIN"}};

inline constexpr std::uint32_t kFaderBanks = hc::kBusCount;   // MAIN + AUX 1..3

struct Widget {
    WidgetKind    kind   = WidgetKind::Knob;
    Rect          rect;                  // hit area / drawn bounds (design units)
    int           param  = -1;           // parameter index, -1 if none
    std::uint32_t index  = 0;            // stem, bus, socket or tab index
    UiAction      action = UiAction::None;
    std::string   caption;
};

struct StripLayout {
    Rect          bed;                   // glass bed of the strip
    bool          isBus = false;
    std::uint32_t index = 0;
    std::string   title;
    Rgba          accent;
    Rect          faderTrack;            // recessed slot; the cap travels inside it
    float         capW = 0, capH = 0;    // fader cap size
    Rect          meter;                 // glass meter window
    Rect          valueBox;              // glass readout under the fader
    int           faderWidget = -1;
};

// Strip accent colours (sRGB): 13 stems, then Main / Aux 1–3.
[[nodiscard]] Rgba stemAccent(std::uint32_t stem) noexcept;
[[nodiscard]] Rgba busAccent(std::uint32_t bus) noexcept;

class Layout {
public:
    // Builds every widget. Throws if a mixer parameter is missing.
    Layout(const params::ParameterTree& tree, float extraH);

    [[nodiscard]] const std::vector<Widget>&      widgets() const noexcept { return widgets_; }
    [[nodiscard]] const std::vector<StripLayout>& strips() const noexcept { return strips_; }
    [[nodiscard]] int   hitTest(float x, float y) const noexcept;   // widget index or -1
    // Stem strip under a point (0-based stem), or -1. Used for drag-and-drop and right-click.
    [[nodiscard]] int   stemStripAt(float x, float y) const noexcept;
    // The title area at the top of a stem strip (name, load state).
    [[nodiscard]] static Rect titleArea(const StripLayout& st) noexcept { return {st.bed.x, st.bed.y, st.bed.w, 140}; }
    [[nodiscard]] float extraH() const noexcept { return extraH_; }

    // Which send the 13 stem faders control (0 = MAIN mix, 1..3 = AUX sends).
    [[nodiscard]] std::uint32_t faderBank() const noexcept { return bank_; }
    void setFaderBank(std::uint32_t bank) noexcept;
    // Rebinds the 5 knobs of every stem strip (parameters and captions).
    void setPage(Page page) noexcept;
    // Knob slots with no parameter on the current page are empty: not drawn, not clickable.
    [[nodiscard]] static bool isEmptySlot(const Widget& w) noexcept {
        return (w.kind == WidgetKind::Knob || w.kind == WidgetKind::SendKnob || w.kind == WidgetKind::Button) && w.param < 0 && w.action == UiAction::None;
    }
    [[nodiscard]] Page page() const noexcept { return page_; }
    [[nodiscard]] int faderParam(std::uint32_t stem, std::uint32_t bank) const noexcept { return stemFader_[stem][bank]; }

    // ANALYZER: shown over the stem strips (the bus strips stay). While it is on, nothing under it
    // takes clicks.
    void setScope(bool on) noexcept { scope_ = on; }
    [[nodiscard]] bool scope() const noexcept { return scope_; }
    [[nodiscard]] Rect mainPanel() const noexcept { return {plate::kMainPanel.x, plate::kMainPanel.y, plate::kMainPanel.w, plate::kMainPanel.h + extraH_}; }
    [[nodiscard]] Rect scopeRect() const noexcept { return mainPanel().inset(14); }
    [[nodiscard]] bool underScope(const Rect& r) const noexcept {
        const Rect m = mainPanel();
        return scope_ && r.x >= m.x && r.right() <= m.right() && r.y >= m.y && r.bottom() <= m.bottom();
    }

    [[nodiscard]] Rect masterMeter() const noexcept { return master_; }
    [[nodiscard]] Rect statusArea() const noexcept { return status_; }
    [[nodiscard]] Rect overview() const noexcept { return overview_; }
    [[nodiscard]] Rect limiterButton() const noexcept { return limit_; }

private:
    void buildSlots(const params::ParameterTree& tree, std::uint32_t stem);
    std::vector<Widget>      widgets_;
    std::vector<StripLayout> strips_;
    std::array<std::array<int, kFaderBanks>, hc::kStemCount> stemFader_{};
    std::uint32_t bank_   = 0;
    Page          page_   = Page::Mix;
    struct Slot { int param = -1; UiAction action = UiAction::None; std::string caption; };   // what a knob / button does on a page
    std::array<std::array<int, kStripKnobs>, hc::kStemCount> knobWidget_{};
    std::array<std::array<int, 3>, hc::kStemCount>           buttonWidget_{};
    std::array<std::array<std::array<Slot, kStripKnobs>, kPages>, hc::kStemCount> knobSlot_{};
    std::array<std::array<std::array<Slot, 3>, kPages>, hc::kStemCount>           buttonSlot_{};
    Rect          limit_;
    float         extraH_ = 0;
    bool          scope_  = false;
    Rect          master_, status_, overview_;
};

// Mouse handling. Pointer positions are in design units.
class Interaction {
public:
    Interaction(Layout& layout, params::ParameterTree& tree) noexcept : layout_(layout), tree_(tree) {}

    // Returns the action of a clicked socket/tab (FaderBank: index in `actionIndex`).
    UiAction mouseDown(float x, float y, bool doubleClick, bool fine, std::uint32_t* actionIndex = nullptr);
    void     mouseMove(float x, float y, bool fine);
    void     mouseUp() noexcept { active_ = -1; pressed_ = -1; seeking_ = false; }
    // Song overview: a click or drag there leaves a seek position (0..1 of the song) to collect.
    [[nodiscard]] bool takeSeek(float& fraction) noexcept {
        if (!hasSeek_) return false;
        hasSeek_ = false;
        fraction = seek_;
        return true;
    }
    void     wheel(float x, float y, float notches, bool fine);
    void     rebind(Layout& layout) noexcept { layout_ = std::ref(layout); active_ = hover_ = pressed_ = -1; }

    [[nodiscard]] int activeWidget() const noexcept { return active_; }
    [[nodiscard]] int hoverWidget() const noexcept { return hover_; }
    [[nodiscard]] int pressedWidget() const noexcept { return pressed_; }   // socket/tab held down

    static constexpr float kKnobUnitsFullRange = 700.0f;   // vertical drag (design units) for 0..1
    static constexpr float kFineDivider        = 10.0f;    // Shift / Ctrl

private:
    std::reference_wrapper<Layout> layout_;
    params::ParameterTree& tree_;
    int   active_  = -1;
    int   hover_   = -1;
    int   pressed_ = -1;
    float lastY_   = 0.0f;
    bool  seeking_ = false;
    bool  hasSeek_ = false;
    float seek_    = 0.0f;
    void  seekAt(float x) noexcept;
};

} // namespace hc::ui
