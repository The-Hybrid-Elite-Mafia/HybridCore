// HEM CONSOLE RIG — implementation.
#include "hybrid/console/ConsoleRig.hpp"

#include "hybrid/console/ColourStage.hpp"
#include "hybrid/console/StepEq.hpp"
#include "hybrid/console/TwinBus.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace hc::console {
namespace {

constexpr int kOrders[ConsoleRig::kOrderCount][3] = {
    {ConsoleRig::UnitStepEq, ConsoleRig::UnitTwinBus, ConsoleRig::UnitColour},
    {ConsoleRig::UnitStepEq, ConsoleRig::UnitColour, ConsoleRig::UnitTwinBus},
    {ConsoleRig::UnitTwinBus, ConsoleRig::UnitStepEq, ConsoleRig::UnitColour},
    {ConsoleRig::UnitTwinBus, ConsoleRig::UnitColour, ConsoleRig::UnitStepEq},
    {ConsoleRig::UnitColour, ConsoleRig::UnitStepEq, ConsoleRig::UnitTwinBus},
    {ConsoleRig::UnitColour, ConsoleRig::UnitTwinBus, ConsoleRig::UnitStepEq},
};
const char* const kUnitKey[] = {"stepeq", "twinbus", "colour"};

std::string chKey(int ch) {
    char b[16];
    std::snprintf(b, sizeof b, "ch%02d", ch + 1);
    return b;
}
std::string num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
    return b;
}
bool parseFloat(std::string_view s, float& out) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    if (s.empty()) return false;
    // from_chars for float is available in MSVC and libstdc++ 11+
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && std::isfinite(out);
}
int findParam(std::span<const fx::ParamInfo> ps, std::string_view id) {
    for (std::size_t i = 0; i < ps.size(); ++i)
        if (id == ps[i].id) return static_cast<int>(i);
    return -1;
}

} // namespace

// The rack behind the desk's MAIN INSERT: the three units in the chosen order. A unit switched out is replaced by a
// delay of its own latency, so the timing of the main bus never jumps when units go in and out.
struct RackInsert final : InsertSlot {
    ConsoleRig::Impl* d = nullptr;
    void process(float* l, float* r, std::uint32_t n) noexcept override;
};

struct ConsoleRig::Impl {
    StudioDesk desk;
    std::array<std::unique_ptr<fx::Effect>, kUnitCount> units{makeStepEq(), makeTwinBus(), makeColourStage()};
    MeterBridge meter;
    std::array<ToolSlot, kChannels> tools;
    ToolSlot colourTool;
    RackInsert rack;
    std::atomic<int> order{0};
    std::array<std::atomic<bool>, kUnitCount> unitIn{};
    std::array<std::array<fx::DelayLine, 2>, kUnitCount> outDelay;   // used while a unit is out
    std::vector<float> mainL, mainR, scratchL, scratchR;
    std::uint32_t maxBlock = 512;
    double fs = 48000.0;

    std::vector<std::pair<std::string, std::pair<fx::Effect*, int>>> effectKeys() {
        std::vector<std::pair<std::string, std::pair<fx::Effect*, int>>> out;
        for (int u = 0; u < kUnitCount; ++u) {
            const auto ps = units[static_cast<std::size_t>(u)]->params();
            for (std::size_t i = 0; i < ps.size(); ++i)
                out.push_back({std::string(kUnitKey[u]) + "." + ps[i].id, {units[static_cast<std::size_t>(u)].get(), static_cast<int>(i)}});
        }
        auto addTools = [&](const std::string& prefix, ToolSlot& slot) {
            for (int t = ToolSlot::Reso; t < ToolSlot::kTypeCount; ++t) {
                fx::Effect& e = slot.tool(t);
                const auto ps = e.params();
                for (std::size_t i = 0; i < ps.size(); ++i)
                    out.push_back({prefix + "." + ToolSlot::typeKey(t) + "." + ps[i].id, {&e, static_cast<int>(i)}});
            }
        };
        for (int c = 0; c < kChannels; ++c) addTools("tool." + chKey(c), tools[static_cast<std::size_t>(c)]);
        addTools("tool.colour", colourTool);
        return out;
    }
};

void RackInsert::process(float* l, float* r, std::uint32_t n) noexcept {
    const int order = d->order.load(std::memory_order_relaxed);
    auto run = [](fx::Effect& e, float* a, float* b, std::uint32_t frames) noexcept {
        for (std::uint32_t done = 0; done < frames;) {
            const std::uint32_t k = std::min(frames - done, fx::Effect::kMaxBlock);
            e.process(a + done, b + done, k);
            done += k;
        }
    };
    for (int pos = 0; pos < 3; ++pos) {
        const int u = kOrders[order][pos];
        fx::Effect& e = *d->units[static_cast<std::size_t>(u)];
        auto& dl = d->outDelay[static_cast<std::size_t>(u)];
        const std::uint32_t lat = e.latency();
        if (d->unitIn[static_cast<std::size_t>(u)].load(std::memory_order_relaxed)) {
            if (lat > 0)   // keep the out-path delay filled with the real signal, so switching OUT is seamless
                for (std::uint32_t i = 0; i < n; ++i) { (void)dl[0].process(l[i], lat); (void)dl[1].process(r[i], lat); }
            run(e, l, r, n);
        } else {
            // OUT: the unit keeps running on a copy (so switching IN is seamless too), the bus gets the dry
            // signal delayed by the unit's latency (the timing never jumps).
            std::copy_n(l, n, d->scratchL.data());
            std::copy_n(r, n, d->scratchR.data());
            run(e, d->scratchL.data(), d->scratchR.data(), n);
            if (lat > 0)
                for (std::uint32_t i = 0; i < n; ++i) { l[i] = dl[0].process(l[i], lat); r[i] = dl[1].process(r[i], lat); }
        }
    }
}

ConsoleRig::ConsoleRig() : d_(std::make_unique<Impl>()) {
    d_->rack.d = d_.get();
    setDefaults();
    prepare(48000.0, 512);
}
ConsoleRig::~ConsoleRig() = default;

void ConsoleRig::prepare(double fs, std::uint32_t maxBlock) {
    Impl& d = *d_;
    d.fs = fs;
    d.maxBlock = std::max<std::uint32_t>(1, maxBlock);
    d.desk.prepare(fs, d.maxBlock);
    for (auto& u : d.units) u->prepare(fs, std::min(d.maxBlock, fx::Effect::kMaxBlock));
    for (int u = 0; u < kUnitCount; ++u)
        for (auto& dl : d.outDelay[static_cast<std::size_t>(u)]) dl.prepare(std::max<std::uint32_t>(1, d.units[static_cast<std::size_t>(u)]->latency()));
    d.meter.prepare(fs, d.maxBlock);
    for (auto& t : d.tools) t.prepare(fs, d.maxBlock);
    d.colourTool.prepare(fs, d.maxBlock);
    d.mainL.assign(d.maxBlock, 0.0f);
    d.mainR.assign(d.maxBlock, 0.0f);
    d.scratchL.assign(d.maxBlock, 0.0f);
    d.scratchR.assign(d.maxBlock, 0.0f);
    d.desk.setMainInsert(&d.rack);
    for (int c = 0; c < kChannels; ++c) d.desk.setChannelInsert(c, &d.tools[static_cast<std::size_t>(c)]);
    colourStageSetInsert(*d.units[UnitColour], &d.colourTool);
}

void ConsoleRig::reset() noexcept {
    Impl& d = *d_;
    d.desk.reset();
    for (auto& u : d.units) u->reset();
    for (auto& a : d.outDelay) for (auto& dl : a) dl.clear();
    d.meter.reset();
    for (auto& t : d.tools) t.reset();
    d.colourTool.reset();
}

void ConsoleRig::process(const Io& io, std::uint32_t frames) noexcept {
    Impl& d = *d_;
    for (std::uint32_t done = 0; done < frames;) {
        const std::uint32_t n = std::min(frames - done, d.maxBlock);
        StudioDesk::Inputs in;
        for (std::size_t c = 0; c < StudioDesk::kChannels; ++c) {
            in.left[c] = io.stemL[c] ? io.stemL[c] + done : nullptr;
            in.right[c] = io.stemR[c] ? io.stemR[c] + done : nullptr;
        }
        StudioDesk::Outputs out;
        out.mainL = io.mainL ? io.mainL + done : d.mainL.data();
        out.mainR = io.mainR ? io.mainR + done : d.mainR.data();
        out.monL = io.monL ? io.monL + done : nullptr;
        out.monR = io.monR ? io.monR + done : nullptr;
        out.altL = io.altL ? io.altL + done : nullptr;
        out.altR = io.altR ? io.altR + done : nullptr;
        d.desk.process(in, out, n);
        d.meter.process(out.mainL, out.mainR, n);
        done += n;
    }
}

StudioDesk& ConsoleRig::desk() noexcept { return d_->desk; }
const StudioDesk& ConsoleRig::desk() const noexcept { return d_->desk; }
fx::Effect& ConsoleRig::unit(int u) noexcept { return *d_->units[static_cast<std::size_t>(std::clamp(u, 0, kUnitCount - 1))]; }
const fx::Effect& ConsoleRig::unit(int u) const noexcept { return *d_->units[static_cast<std::size_t>(std::clamp(u, 0, kUnitCount - 1))]; }
MeterBridge& ConsoleRig::meter() noexcept { return d_->meter; }
ToolSlot& ConsoleRig::channelTool(int ch) noexcept { return d_->tools[static_cast<std::size_t>(std::clamp(ch, 0, kChannels - 1))]; }
const ToolSlot& ConsoleRig::channelTool(int ch) const noexcept { return d_->tools[static_cast<std::size_t>(std::clamp(ch, 0, kChannels - 1))]; }
ToolSlot& ConsoleRig::colourTool() noexcept { return d_->colourTool; }
const ToolSlot& ConsoleRig::colourTool() const noexcept { return d_->colourTool; }

void ConsoleRig::setRackOrder(int o) noexcept { d_->order.store(std::clamp(o, 0, kOrderCount - 1), std::memory_order_relaxed); }
int ConsoleRig::rackOrder() const noexcept { return d_->order.load(std::memory_order_relaxed); }
void ConsoleRig::setUnitIn(int u, bool on) noexcept {
    if (u >= 0 && u < kUnitCount) d_->unitIn[static_cast<std::size_t>(u)].store(on, std::memory_order_relaxed);
}
bool ConsoleRig::unitIn(int u) const noexcept {
    return u >= 0 && u < kUnitCount && d_->unitIn[static_cast<std::size_t>(u)].load(std::memory_order_relaxed);
}
int ConsoleRig::rackUnitAt(int order, int pos) noexcept { return kOrders[std::clamp(order, 0, kOrderCount - 1)][std::clamp(pos, 0, 2)]; }
const char* ConsoleRig::unitName(int u) noexcept {
    static const char* const n[] = {"STEP EQ", "TWIN BUS", "COLOUR STAGE"};
    return n[std::clamp(u, 0, kUnitCount - 1)];
}
std::string ConsoleRig::orderName(int order) {
    static const char* const s[] = {"EQ", "BUS", "COLOUR"};
    std::string out;
    for (int p = 0; p < 3; ++p) {
        if (p) out += " > ";
        out += s[rackUnitAt(order, p)];
    }
    return out;
}
std::uint32_t ConsoleRig::rackLatency() const noexcept {
    std::uint32_t l = 0;
    for (const auto& u : d_->units) l += u->latency();   // in or out, the latency is the same (see RackInsert)
    return l;
}

void ConsoleRig::setDefaults() noexcept {
    Impl& d = *d_;
    d.desk.setDefaults();
    for (auto& u : d.units) u->setDefaults();
    d.order.store(0);
    for (auto& a : d.unitIn) a.store(true);
    for (auto& t : d.tools) {
        t.setType(ToolSlot::Off);
        for (int k = ToolSlot::Reso; k < ToolSlot::kTypeCount; ++k) t.tool(k).setDefaults();
    }
    d.colourTool.setType(ToolSlot::Off);
    for (int k = ToolSlot::Reso; k < ToolSlot::kTypeCount; ++k) d.colourTool.tool(k).setDefaults();
}

std::string ConsoleRig::saveText() const {
    Impl& d = *d_;
    std::string s;
    s.reserve(32768);
    auto line = [&](const std::string& k, float v) { s += k; s += '='; s += num(v); s += '\n'; };
    const auto cps = StudioDesk::channelParams();
    for (int c = 0; c < kChannels; ++c)
        for (std::size_t i = 0; i < cps.size(); ++i) line("desk." + chKey(c) + "." + cps[i].id, d.desk.channel(c, static_cast<int>(i)));
    const auto mps = StudioDesk::masterParams();
    for (std::size_t i = 0; i < mps.size(); ++i) line(std::string("desk.") + mps[i].id, d.desk.master(static_cast<int>(i)));
    line("rack.order", static_cast<float>(rackOrder()));
    for (int u = 0; u < kUnitCount; ++u) line(std::string("rack.in_") + kUnitKey[u], unitIn(u) ? 1.0f : 0.0f);
    for (int c = 0; c < kChannels; ++c) line("tool." + chKey(c) + ".type", static_cast<float>(d.tools[static_cast<std::size_t>(c)].type()));
    line("tool.colour.type", static_cast<float>(d.colourTool.type()));
    for (const auto& [k, ref] : d.effectKeys()) line(k, ref.first->get(ref.second));
    return s;
}

bool ConsoleRig::setByKey(std::string_view key, float v) noexcept {
    Impl& d = *d_;
    if (key.starts_with("desk.")) {
        std::string_view rest = key.substr(5);
        if (rest.size() > 5 && rest.starts_with("ch") && rest[4] == '.') {
            int ch = 0;
            if (std::from_chars(rest.data() + 2, rest.data() + 4, ch).ec != std::errc() || ch < 1 || ch > kChannels) return false;
            const int i = findParam(StudioDesk::channelParams(), rest.substr(5));
            if (i < 0) return false;
            d.desk.setChannel(ch - 1, i, v);
            return true;
        }
        const int i = findParam(StudioDesk::masterParams(), rest);
        if (i < 0) return false;
        d.desk.setMaster(i, v);
        return true;
    }
    if (key == "rack.order") { setRackOrder(static_cast<int>(std::lround(v))); return true; }
    for (int u = 0; u < kUnitCount; ++u)
        if (key == std::string("rack.in_") + kUnitKey[u]) { setUnitIn(u, v > 0.5f); return true; }
    if (key.starts_with("tool.") && key.ends_with(".type")) {
        const std::string_view who = key.substr(5, key.size() - 5 - 5);
        if (who == "colour") { d.colourTool.setType(static_cast<int>(std::lround(v))); return true; }
        int ch = 0;
        if (who.size() == 4 && who.starts_with("ch") && std::from_chars(who.data() + 2, who.data() + 4, ch).ec == std::errc() && ch >= 1 && ch <= kChannels) {
            d.tools[static_cast<std::size_t>(ch - 1)].setType(static_cast<int>(std::lround(v)));
            return true;
        }
        return false;
    }
    for (const auto& [k, ref] : d.effectKeys())
        if (k == key) { ref.first->set(ref.second, v); return true; }
    return false;
}

bool ConsoleRig::getByKey(std::string_view key, float& v) const noexcept {
    const std::string all = saveText();
    const std::string want = std::string(key) + "=";
    std::size_t pos = 0;
    while (pos < all.size()) {
        const std::size_t e = all.find('\n', pos);
        const std::string_view ln(all.data() + pos, (e == std::string::npos ? all.size() : e) - pos);
        if (ln.starts_with(want)) return parseFloat(ln.substr(want.size()), v);
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return false;
}

int ConsoleRig::loadText(std::string_view text) {
    int applied = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t e = text.find('\n', pos);
        std::string_view ln = text.substr(pos, (e == std::string_view::npos ? text.size() : e) - pos);
        const std::size_t eq = ln.find('=');
        if (eq != std::string_view::npos && !ln.starts_with("#")) {
            float v = 0;
            std::string_view k = ln.substr(0, eq);
            while (!k.empty() && k.back() == ' ') k.remove_suffix(1);
            if (parseFloat(ln.substr(eq + 1), v) && setByKey(k, v)) ++applied;
        }
        if (e == std::string_view::npos) break;
        pos = e + 1;
    }
    return applied;
}

} // namespace hc::console
