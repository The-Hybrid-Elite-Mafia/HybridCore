#include "hybrid/engine/FxRack.hpp"
#include "hybrid/fx/Effects.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace hc::engine {
namespace {
constexpr std::uint32_t kDrySize = FxRack::kMaxLatency + fx::Effect::kMaxBlock + 8;   // bypass line: latency + one chunk
constexpr std::uint32_t kPdcSize = FxRack::kMaxLatency + fx::Effect::kMaxBlock + 8;
} // namespace

FxRack::FxRack() {
    for (auto& row : types_) row.fill(-1);
    for (auto& row : msg_) row.fill(nullptr);
    for (auto& row : bypass_) for (auto& b : row) b.store(false, std::memory_order_relaxed);
    for (auto& row : slots_)
        for (Slot& s : row) { s.dry[0].assign(kDrySize, 0.0f); s.dry[1].assign(kDrySize, 0.0f); }
    for (Pdc& p : pdc_) { p.buf[0].assign(kPdcSize, 0.0f); p.buf[1].assign(kPdcSize, 0.0f); }
    aux_.buf[0].assign(kPdcSize, 0.0f);
    aux_.buf[1].assign(kPdcSize, 0.0f);
    stemLat_.fill(0);
}

FxRack::~FxRack() {
    // Message thread, after the audio thread let go of the rack: everything still held is ours.
    Command c;
    while (commands_.tryPop(c)) delete c.effect;
    collectRetired();
    for (auto& row : slots_)
        for (Slot& s : row) { delete s.fx; s.fx = nullptr; }
}

void FxRack::prepare(double sampleRate, std::uint32_t maxFrames) {
    fs_ = sampleRate;
    maxFrames_ = maxFrames;
    fadeStep_ = static_cast<float>(1.0 / (0.010 * sampleRate));
}

// ---- message thread -------------------------------------------------------------------------
bool FxRack::setEffect(std::uint32_t stem, int slot, int type) {
    if (stem >= kStems || slot < 0 || slot >= kSlots) return false;
    const auto list = fx::effects();
    std::unique_ptr<fx::Effect> e;
    if (type >= 0 && type < static_cast<int>(list.size())) {
        e = list[static_cast<std::size_t>(type)].create();
        e->prepare(fs_, fx::Effect::kMaxBlock);
        e->setTempo(tempo_.load(std::memory_order_relaxed));
    } else {
        type = -1;
    }
    if (!commands_.tryPush(Command{stem, slot, e.get()})) return false;
    msg_[stem][static_cast<std::size_t>(slot)] = e.release();
    types_[stem][static_cast<std::size_t>(slot)] = type;
    bypass_[stem][static_cast<std::size_t>(slot)].store(false, std::memory_order_relaxed);
    return true;
}

int FxRack::type(std::uint32_t stem, int slot) const noexcept {
    return stem < kStems && slot >= 0 && slot < kSlots ? types_[stem][static_cast<std::size_t>(slot)] : -1;
}
fx::Effect* FxRack::effect(std::uint32_t stem, int slot) const noexcept {
    return stem < kStems && slot >= 0 && slot < kSlots ? msg_[stem][static_cast<std::size_t>(slot)] : nullptr;
}
void FxRack::setBypass(std::uint32_t stem, int slot, bool on) noexcept {
    if (stem < kStems && slot >= 0 && slot < kSlots) bypass_[stem][static_cast<std::size_t>(slot)].store(on, std::memory_order_relaxed);
}
bool FxRack::bypass(std::uint32_t stem, int slot) const noexcept {
    return stem < kStems && slot >= 0 && slot < kSlots && bypass_[stem][static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
}
bool FxRack::anyEffect() const noexcept {
    for (const auto& row : types_)
        for (int t : row) if (t >= 0) return true;
    return false;
}
int FxRack::count(std::uint32_t stem) const noexcept {
    int n = 0;
    if (stem < kStems) for (int t : types_[stem]) n += t >= 0;
    return n;
}
void FxRack::setTempo(double bpm) noexcept {
    tempo_.store(static_cast<float>(bpm), std::memory_order_relaxed);
    for (const auto& row : msg_)
        for (fx::Effect* e : row) if (e) e->setTempo(bpm);
}
void FxRack::clear() {
    for (std::uint32_t s = 0; s < kStems; ++s)
        for (int k = 0; k < kSlots; ++k)
            if (types_[s][static_cast<std::size_t>(k)] >= 0) (void)setEffect(s, k, -1);
}
void FxRack::collectRetired() noexcept {
    fx::Effect* e = nullptr;
    while (retired_.tryPop(e)) delete e;
}
std::uint32_t FxRack::plannedLatency() const noexcept {
    std::uint32_t mx = 0;
    for (const auto& row : msg_) {
        std::uint32_t l = 0;
        for (const fx::Effect* e : row) if (e) l += e->latency();
        mx = std::max(mx, l);
    }
    return std::min(mx, kMaxLatency);
}

std::string FxRack::toText() const {
    std::string out;
    const auto list = fx::effects();
    char key[32], num[48];
    for (std::uint32_t s = 0; s < kStems; ++s)
        for (int k = 0; k < kSlots; ++k) {
            const int t = types_[s][static_cast<std::size_t>(k)];
            const fx::Effect* e = msg_[s][static_cast<std::size_t>(k)];
            if (t < 0 || !e) continue;
            std::snprintf(key, sizeof key, "stem%02u.fx%d=", s + 1, k + 1);
            out += key;
            out += list[static_cast<std::size_t>(t)].name;
            out += bypass(s, k) ? "|bypass=1" : "|bypass=0";
            const auto ps = e->params();
            for (std::size_t i = 0; i < ps.size(); ++i) {
                std::snprintf(num, sizeof num, "%.7g", static_cast<double>(e->get(static_cast<int>(i))));
                out += std::string("|") + ps[i].id + "=" + num;
            }
            out += "\n";
        }
    return out;
}

std::string FxRack::fromText(const std::string& text) {
    clear();
    std::string skipped;
    const auto list = fx::effects();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        unsigned stem = 0;
        int slot = 0;
        if (std::sscanf(line.c_str(), "stem%u.fx%d=", &stem, &slot) != 2 || stem < 1 || stem > kStems || slot < 1 || slot > kSlots) continue;
        const auto eq = line.find('=');
        std::vector<std::string> parts;
        std::string cur;
        for (std::size_t i = eq + 1; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == '|') { parts.push_back(cur); cur.clear(); }
            else if (line[i] != '\r') cur.push_back(line[i]);
        }
        if (parts.empty()) continue;
        int type = -1;
        for (std::size_t t = 0; t < list.size(); ++t) if (parts[0] == list[t].name) type = static_cast<int>(t);
        if (type < 0) { skipped += (skipped.empty() ? "" : ", ") + parts[0]; continue; }
        if (!setEffect(stem - 1, slot - 1, type)) continue;
        fx::Effect* e = effect(stem - 1, slot - 1);
        const auto ps = e->params();
        for (std::size_t p = 1; p < parts.size(); ++p) {
            const auto q = parts[p].find('=');
            if (q == std::string::npos) continue;
            const std::string id = parts[p].substr(0, q);
            const float v = std::strtof(parts[p].c_str() + q + 1, nullptr);
            if (id == "bypass") { setBypass(stem - 1, slot - 1, v >= 0.5f); continue; }
            for (std::size_t i = 0; i < ps.size(); ++i)
                if (id == ps[i].id) e->set(static_cast<int>(i), v);
        }
    }
    return skipped.empty() ? std::string() : "effects not found (newer version?): " + skipped;
}

// ---- audio thread ---------------------------------------------------------------------------
void FxRack::beginBlock() noexcept {
    for (std::size_t k = 0; k < 64; ++k) {
        if (retired_.sizeApprox() + 1 >= 128) break;   // keep room for the effect being replaced
        Command c;
        if (!commands_.tryPop(c)) break;
        Slot& s = slots_[c.stem][static_cast<std::size_t>(c.slot)];
        fx::Effect* old = s.fx;
        s.fx = c.effect;
        s.mix = bypass_[c.stem][static_cast<std::size_t>(c.slot)].load(std::memory_order_relaxed) ? 0.0f : 1.0f;
        std::fill(s.dry[0].begin(), s.dry[0].end(), 0.0f);   // RT-safe: no allocation, bounded work
        std::fill(s.dry[1].begin(), s.dry[1].end(), 0.0f);
        if (old) (void)retired_.tryPush(old);
    }
    std::uint32_t mx = 0;
    for (std::uint32_t st = 0; st < kStems; ++st) {
        std::uint32_t l = 0;
        for (const Slot& s : slots_[st]) if (s.fx) l += s.fx->latency();
        stemLat_[st] = std::min(l, kMaxLatency);
        mx = std::max(mx, stemLat_[st]);
    }
    maxLat_ = mx;
    latencyPub_.store(mx, std::memory_order_relaxed);
}

bool FxRack::active(std::uint32_t stem) noexcept {
    if (maxLat_ > 0) return true;
    for (const Slot& s : slots_[stem]) if (s.fx) return true;
    return false;
}

void FxRack::runSlot(Slot& s, std::uint32_t stem, int slot, float* L, float* R, std::uint32_t n) noexcept {
    const std::uint32_t lat = std::min(s.fx->latency(), kMaxLatency);
    const auto size = static_cast<std::uint32_t>(s.dry[0].size());
    const std::uint32_t start = s.dryPos;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t w = (start + i) % size;
        s.dry[0][w] = L[i];
        s.dry[1][w] = R[i];
    }
    s.dryPos = (start + n) % size;
    s.fx->process(L, R, n);
    const float target = bypass_[stem][static_cast<std::size_t>(slot)].load(std::memory_order_relaxed) ? 0.0f : 1.0f;
    if (s.mix == 1.0f && target == 1.0f) return;
    for (std::uint32_t i = 0; i < n; ++i) {
        s.mix = target > s.mix ? std::min(target, s.mix + fadeStep_) : std::max(target, s.mix - fadeStep_);
        const std::uint32_t r = (start + i + size - lat) % size;
        L[i] = s.mix * L[i] + (1.0f - s.mix) * s.dry[0][r];
        R[i] = s.mix * R[i] + (1.0f - s.mix) * s.dry[1][r];
    }
}

void FxRack::process(std::uint32_t stem, float* L, float* R, std::uint32_t frames) noexcept {
    if (stem >= kStems) return;
    for (std::uint32_t done = 0; done < frames;) {
        const std::uint32_t n = std::min(frames - done, fx::Effect::kMaxBlock);
        for (int k = 0; k < kSlots; ++k) {
            Slot& s = slots_[stem][static_cast<std::size_t>(k)];
            if (s.fx) runSlot(s, stem, k, L + done, R + done, n);
        }
        done += n;
    }
    // delay compensation: line this stem up with the slowest one
    Pdc& p = pdc_[stem];
    const std::uint32_t d = maxLat_ - stemLat_[stem];
    const auto size = static_cast<std::uint32_t>(p.buf[0].size());
    for (std::uint32_t i = 0; i < frames; ++i) {
        p.buf[0][p.pos] = L[i];
        p.buf[1][p.pos] = R[i];
        const std::uint32_t r = (p.pos + size - d) % size;
        L[i] = p.buf[0][r];
        R[i] = p.buf[1][r];
        p.pos = (p.pos + 1) % size;
    }
}

void FxRack::delayAux(int channel, float* x, std::uint32_t frames) noexcept {
    if (channel < 0 || channel > 1) return;
    std::vector<float>& b = aux_.buf[static_cast<std::size_t>(channel)];
    const auto size = static_cast<std::uint32_t>(b.size());
    std::uint32_t pos = aux_.pos;
    for (std::uint32_t i = 0; i < frames; ++i) {
        b[pos] = x[i];
        x[i] = b[(pos + size - maxLat_) % size];
        pos = (pos + 1) % size;
    }
    if (channel == 1) aux_.pos = pos;   // both channels advance together (0 then 1 every block)
}

} // namespace hc::engine
