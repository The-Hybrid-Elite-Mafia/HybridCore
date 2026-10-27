// HEM DIGITAL CONSOLE — processor, export and session file.
#include "hybrid/console/ConsolePlayer.hpp"

#include "hybrid/console/ColourStage.hpp"
#include "hybrid/media/StemLoader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace hc::console {

// =====================================================================================================
//  ConsoleProcessor
// =====================================================================================================
ConsoleProcessor::~ConsoleProcessor() {
    for (auto*& s : slots_) { delete s; s = nullptr; }
    Command c{};
    while (commands_.tryPop(c)) delete c.audio;
    collectRetired();
}

bool ConsoleProcessor::submit(int slot, std::unique_ptr<engine::StemAudio>& audio) noexcept {
    if (slot < 0 || slot >= kPlayerSlots) return false;
    if (!commands_.tryPush(Command{slot, audio.get()})) return false;
    audio.release();   // owned by the queue / the audio thread now
    return true;
}

void ConsoleProcessor::collectRetired() noexcept {
    engine::StemAudio* s = nullptr;
    while (retired_.tryPop(s)) delete s;
}

void ConsoleProcessor::prepare(const io::StreamConfig& cfg) {
    maxFrames_ = std::max<std::uint32_t>(1, cfg.blockFrames);
    rate_ = cfg.sampleRate ? cfg.sampleRate : 48000;
    rig_.prepare(static_cast<double>(rate_), maxFrames_);
    stem_.assign(static_cast<std::size_t>(kPlayerSlots) * 2 * maxFrames_, 0.0f);
    for (auto* v : {&mainL_, &mainR_, &monL_, &monR_, &altL_, &altR_, &c1L_, &c1R_, &c2L_, &c2R_}) v->assign(maxFrames_, 0.0f);
}

void ConsoleProcessor::drain() noexcept {
    bool changed = false;
    for (std::size_t k = 0; k < kQueue; ++k) {
        if (retired_.sizeApprox() >= kRetire) break;   // only take a command when the old stem has a place to go
        Command c{};
        if (!commands_.tryPop(c)) break;
        engine::StemAudio* old = slots_[static_cast<std::size_t>(c.slot)];
        slots_[static_cast<std::size_t>(c.slot)] = c.audio;
        if (old) (void)retired_.tryPush(old);
        changed = true;
    }
    if (changed) {
        song_ = 0;
        for (const auto* s : slots_) if (s) song_ = std::max(song_, s->frames);
        songPub_.store(song_, std::memory_order_relaxed);
    }
}

namespace {
constexpr std::uint32_t kFadeIn = 64;
void fadeIn(float* x, std::uint32_t n) noexcept {
    const std::uint32_t m = std::min(n, kFadeIn);
    for (std::uint32_t i = 0; i < m; ++i) x[i] *= static_cast<float>(i) / static_cast<float>(kFadeIn);
}
void fadeOut(float* x, std::uint32_t n) noexcept {
    for (std::uint32_t i = 0; i < n; ++i) x[i] *= 1.0f - static_cast<float>(i + 1) / static_cast<float>(n);
}
} // namespace

bool ConsoleProcessor::render(std::uint32_t frames) noexcept {
    if (song_ == 0) { playing_ = false; playhead_ = 0; seekReq_.store(-1, std::memory_order_relaxed); return false; }
    const std::int64_t seek = seekReq_.exchange(-1, std::memory_order_acq_rel);
    auto clampSeek = [&] { return std::min<std::uint64_t>(static_cast<std::uint64_t>(seek), song_ - 1); };
    const bool want = wantPlay_.load(std::memory_order_acquire);
    bool startFade = false, stopping = false;
    if (want && !playing_) { playing_ = true; startFade = true; }
    else if (!want && playing_) stopping = true;
    if (seek >= 0 && !stopping) { playhead_ = clampSeek(); startFade = playing_; }
    if (!playing_) return false;

    std::uint64_t pos = playhead_;
    std::uint32_t done = 0, endFrame = frames;
    bool ended = false;
    while (done < frames) {
        const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames - done, song_ - pos));
        for (int s = 0; s < kPlayerSlots; ++s) {
            float* L = stem_.data() + static_cast<std::size_t>(s) * 2 * maxFrames_ + done;
            float* R = L + maxFrames_;
            const engine::StemAudio* a = slots_[static_cast<std::size_t>(s)];
            std::uint32_t have = 0;
            if (a && pos < a->frames) {
                have = static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk, a->frames - pos));
                std::memcpy(L, a->left.data() + pos, sizeof(float) * have);
                std::memcpy(R, a->right.data() + pos, sizeof(float) * have);
            }
            if (have < chunk) { std::memset(L + have, 0, sizeof(float) * (chunk - have)); std::memset(R + have, 0, sizeof(float) * (chunk - have)); }
        }
        done += chunk;
        pos += chunk;
        if (pos >= song_) {
            pos = 0;
            if (!loop_.load(std::memory_order_relaxed)) { ended = true; endFrame = done; break; }
        }
    }
    for (int s = 0; s < kPlayerSlots * 2; ++s) {
        float* x = stem_.data() + static_cast<std::size_t>(s) * maxFrames_;
        if (endFrame < frames) std::memset(x + endFrame, 0, sizeof(float) * (frames - endFrame));
        if (startFade) fadeIn(x, frames);
        if (stopping) fadeOut(x, frames);
    }
    playhead_ = pos;
    if (stopping) { playing_ = false; if (seek >= 0) playhead_ = clampSeek(); }
    if (ended) { playing_ = false; wantPlay_.store(false, std::memory_order_release); }
    return true;
}

void ConsoleProcessor::process(const engine::ProcessContext& ctx) noexcept {
    for (std::uint32_t c = 0; c < ctx.numOutputs; ++c)
        if (ctx.outputs[c]) std::memset(ctx.outputs[c], 0, sizeof(float) * ctx.frames);
    const std::uint32_t n = ctx.frames;
    if (n == 0 || n > maxFrames_) return;
    drain();
    const bool have = render(n);
    ConsoleRig::Io io;
    for (int s = 0; s < kPlayerSlots; ++s) {
        const float* L = have && slots_[static_cast<std::size_t>(s)] ? stem_.data() + static_cast<std::size_t>(s) * 2 * maxFrames_ : nullptr;
        const float* R = L ? L + maxFrames_ : nullptr;
        if (s < ConsoleRig::kChannels) { io.stemL[static_cast<std::size_t>(s)] = L; io.stemR[static_cast<std::size_t>(s)] = R; }
        else if (s == kExt1) { io.ext1L = L; io.ext1R = R; }
        else { io.ext2L = L; io.ext2R = R; }
    }
    io.mainL = mainL_.data(); io.mainR = mainR_.data();
    io.monL = monL_.data(); io.monR = monR_.data(); io.altL = altL_.data(); io.altR = altR_.data();
    io.cue1L = c1L_.data(); io.cue1R = c1R_.data(); io.cue2L = c2L_.data(); io.cue2R = c2R_.data();
    rig_.process(io, n);
    const int l = listen_.load(std::memory_order_relaxed);
    const float* srcL = l == ListenCue1 ? c1L_.data() : l == ListenCue2 ? c2L_.data() : monL_.data();
    const float* srcR = l == ListenCue1 ? c1R_.data() : l == ListenCue2 ? c2R_.data() : monR_.data();
    const bool addAlt = l == ListenMonitor;   // one output device: the ALT feed plays on it too
    if (ctx.numOutputs >= 2 && ctx.outputs[0] && ctx.outputs[1]) {
        for (std::uint32_t i = 0; i < n; ++i) {
            ctx.outputs[0][i] = srcL[i] + (addAlt ? altL_[i] : 0.0f);
            ctx.outputs[1][i] = srcR[i] + (addAlt ? altR_[i] : 0.0f);
        }
    } else if (ctx.numOutputs == 1 && ctx.outputs[0]) {
        for (std::uint32_t i = 0; i < n; ++i) ctx.outputs[0][i] = 0.5f * (srcL[i] + srcR[i] + (addAlt ? altL_[i] + altR_[i] : 0.0f));
    }
    playheadPub_.store(playhead_, std::memory_order_relaxed);
    playingPub_.store(playing_, std::memory_order_relaxed);
}

// =====================================================================================================
//  WAV writer that streams (header patched at the end)
// =====================================================================================================
namespace {

class WavStream {
public:
    bool open(const std::filesystem::path& p, bool float32) {
        f_.open(p, std::ios::binary | std::ios::trunc);
        if (!f_) return false;
        float_ = float32;
        path_ = p;
        writeHeader(0);
        return static_cast<bool>(f_);
    }
    void write(const float* l, const float* r, std::uint32_t n, float& peak, std::uint64_t& clipped) {
        buf_.resize(static_cast<std::size_t>(n) * (float_ ? 8 : 6));
        std::uint8_t* o = buf_.data();
        for (std::uint32_t i = 0; i < n; ++i) {
            for (float v : {l[i], r[i]}) {
                const float a = std::fabs(v);
                peak = std::max(peak, a);
                if (a > 1.0f) ++clipped;
                if (float_) { std::memcpy(o, &v, 4); o += 4; }
                else {
                    const auto s = static_cast<std::int32_t>(std::lround(std::clamp(static_cast<double>(v), -1.0, 1.0) * 8388607.0));
                    o[0] = static_cast<std::uint8_t>(s & 0xFF); o[1] = static_cast<std::uint8_t>((s >> 8) & 0xFF); o[2] = static_cast<std::uint8_t>((s >> 16) & 0xFF);
                    o += 3;
                }
            }
        }
        f_.write(reinterpret_cast<const char*>(buf_.data()), static_cast<std::streamsize>(buf_.size()));
        frames_ += n;
    }
    bool close() {
        if (!f_.is_open()) return false;
        f_.seekp(0);
        writeHeader(frames_);
        const bool ok = static_cast<bool>(f_);
        f_.close();
        return ok;
    }
    void discard() { if (f_.is_open()) f_.close(); std::error_code ec; std::filesystem::remove(path_, ec); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    void put32(std::uint32_t v) { const char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16), static_cast<char>(v >> 24)}; f_.write(b, 4); }
    void put16(std::uint16_t v) { const char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)}; f_.write(b, 2); }
    void writeHeader(std::uint64_t frames) {
        const std::uint16_t bytes = float_ ? 4 : 3, ch = 2;
        const auto dataBytes = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames * ch * bytes, 0xFFFFFFFFull - 64));
        f_.write("RIFF", 4); put32(36 + dataBytes); f_.write("WAVE", 4);
        f_.write("fmt ", 4); put32(16); put16(float_ ? 3 : 1); put16(ch); put32(hc::kSampleRate); put32(hc::kSampleRate * ch * bytes);
        put16(static_cast<std::uint16_t>(ch * bytes)); put16(static_cast<std::uint16_t>(bytes * 8));
        f_.write("data", 4); put32(dataBytes);
    }
    std::ofstream f_;
    std::filesystem::path path_;
    std::vector<std::uint8_t> buf_;
    std::uint64_t frames_ = 0;
    bool float_ = false;
};

std::string safeName(std::string s) {
    for (char& c : s) if (std::strchr("\\/:*?\"<>|", c) || static_cast<unsigned char>(c) < 32) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? std::string("stem") : s;
}

bool echoEngaged(ConsoleRig& rig) {
    for (int c = 0; c < ConsoleRig::kChannels; ++c)
        if (rig.desk().channel(c, StudioDesk::Insert) > 0.5f && rig.channelTool(c).type() == ToolSlot::Echo) return true;
    return rig.desk().master(StudioDesk::MainInsert) > 0.5f && rig.unitIn(ConsoleRig::UnitColour) &&
           rig.colour().get(colour_ids::Insert) > 0.5f && rig.colourTool().type() == ToolSlot::Echo;
}

} // namespace

std::expected<ExportResult, std::string> renderExport(const ExportJob& job, const std::array<RenderSource, kPlayerSlots>& src, const ExportProgress& progress) {
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t song = 0;
    for (const RenderSource& s : src) if (s.left) song = std::max(song, s.frames);
    if (song == 0) return std::unexpected(std::string("No stems loaded"));
    constexpr std::uint32_t kBlock = 1024;
    auto rig = std::make_unique<ConsoleRig>();
    rig->loadText(job.rigState);
    rig->prepare(static_cast<double>(hc::kSampleRate), kBlock);
    rig->reset();
    const bool mix = job.kind == ExportJob::Kind::Mix;
    const std::uint32_t latency = mix && rig->desk().master(StudioDesk::MainInsert) > 0.5f ? rig->rackLatency() : 0;
    const std::uint64_t tail = echoEngaged(*rig) ? 2ull * hc::kSampleRate : 0;
    const std::uint64_t outFrames = song + tail;
    const std::uint64_t total = outFrames + latency;

    ExportResult res;
    res.latencyTrimmed = latency;
    std::vector<WavStream> writers;
    std::vector<int> stemCh;
    if (mix) {
        writers.emplace_back();
        if (!writers.back().open(job.out, job.float32)) return std::unexpected("Can't write " + job.out.string());
    } else {
        std::error_code ec;
        std::filesystem::create_directories(job.out, ec);
        for (int c = 0; c < ConsoleRig::kChannels; ++c) {
            if (!src[static_cast<std::size_t>(c)].left) continue;
            char num[8];
            std::snprintf(num, sizeof num, "%02d ", c + 1);
            const std::string nm = job.names[static_cast<std::size_t>(c)].empty() ? std::string("channel") : job.names[static_cast<std::size_t>(c)];
            writers.emplace_back();
            const auto p = job.out / (std::string(num) + safeName(nm) + ".wav");
            if (!writers.back().open(p, job.float32)) {
                for (auto& w : writers) w.discard();
                return std::unexpected("Can't write " + p.string());
            }
            stemCh.push_back(c);
        }
    }

    std::vector<float> in(static_cast<std::size_t>(kPlayerSlots) * 2 * kBlock, 0.0f);
    std::vector<float> mL(kBlock), mR(kBlock);
    std::array<std::vector<float>, ConsoleRig::kChannels> dL, dR;
    if (!mix) for (int c : stemCh) { dL[static_cast<std::size_t>(c)].assign(kBlock, 0.0f); dR[static_cast<std::size_t>(c)].assign(kBlock, 0.0f); }
    float peak = 0;
    std::uint64_t written = 0;
    int lastPct = -1;
    for (std::uint64_t pos = 0; pos < total; pos += kBlock) {
        const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(kBlock, total - pos));
        ConsoleRig::Io io;
        for (int s = 0; s < kPlayerSlots; ++s) {
            const RenderSource& rs = src[static_cast<std::size_t>(s)];
            float* L = in.data() + static_cast<std::size_t>(s) * 2 * kBlock;
            float* R = L + kBlock;
            const float* useL = nullptr;
            const float* useR = nullptr;
            if (rs.left && pos < rs.frames) {
                const auto have = static_cast<std::uint32_t>(std::min<std::uint64_t>(n, rs.frames - pos));
                std::memcpy(L, rs.left + pos, sizeof(float) * have);
                std::memcpy(R, (rs.right ? rs.right : rs.left) + pos, sizeof(float) * have);
                if (have < n) { std::memset(L + have, 0, sizeof(float) * (n - have)); std::memset(R + have, 0, sizeof(float) * (n - have)); }
                useL = L; useR = R;
            }
            if (s < ConsoleRig::kChannels) { io.stemL[static_cast<std::size_t>(s)] = useL; io.stemR[static_cast<std::size_t>(s)] = useR; }
            else if (s == kExt1) { io.ext1L = useL; io.ext1R = useR; }
            else { io.ext2L = useL; io.ext2R = useR; }
        }
        io.mainL = mL.data(); io.mainR = mR.data();
        if (!mix) for (int c : stemCh) { io.directL[static_cast<std::size_t>(c)] = dL[static_cast<std::size_t>(c)].data(); io.directR[static_cast<std::size_t>(c)] = dR[static_cast<std::size_t>(c)].data(); }
        rig->process(io, n);
        // the first `latency` samples of the main out are the rack's delay: skipped, so the file starts on time
        const std::uint64_t skip = pos < latency ? std::min<std::uint64_t>(n, latency - pos) : 0;
        const auto keep = static_cast<std::uint32_t>(std::min<std::uint64_t>(n - skip, outFrames - written));
        if (keep > 0) {
            if (mix) {
                writers[0].write(mL.data() + skip, mR.data() + skip, keep, peak, res.clipped);
            } else {
                float stemPeak = 0;
                for (std::size_t k = 0; k < stemCh.size(); ++k) {
                    const auto c = static_cast<std::size_t>(stemCh[k]);
                    writers[k].write(dL[c].data() + skip, dR[c].data() + skip, keep, stemPeak, res.clipped);
                }
                for (std::uint32_t i = 0; i < keep; ++i) peak = std::max({peak, std::fabs(mL[skip + i]), std::fabs(mR[skip + i])});
            }
            written += keep;
        }
        const int pct = static_cast<int>(100.0 * static_cast<double>(pos + n) / static_cast<double>(total));
        if (progress && pct != lastPct) {
            lastPct = pct;
            if (!progress(static_cast<float>(pos + n) / static_cast<float>(total))) {
                for (auto& w : writers) w.discard();
                return std::unexpected(std::string("Export cancelled"));
            }
        }
    }
    for (auto& w : writers) {
        if (!w.close()) return std::unexpected("Writing " + w.path().string() + " failed (disk full?)");
        res.files.push_back(w.path());
    }
    res.frames = outFrames;
    res.peakDb = peak > 0 ? 20.0f * std::log10(peak) : -144.0f;
    res.integratedLufs = rig->meter().integratedLufs();
    res.truePeakDbtp = std::max(rig->meter().truePeakDbtp(0), rig->meter().truePeakDbtp(1));
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (res.clipped > 0 && !job.float32) res.warning = "Some samples were over full scale and were clipped in the file";
    return res;
}

ConsoleExporter::~ConsoleExporter() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

bool ConsoleExporter::start(ExportJob job) {
    if (busy_.load()) return false;
    if (thread_.joinable()) thread_.join();
    busy_.store(true); cancel_.store(false); done_.store(false); progress_.store(0.0f);
    thread_ = std::thread([this, job = std::move(job)] {
        std::array<std::unique_ptr<engine::StemAudio>, kPlayerSlots> audio;
        std::array<RenderSource, kPlayerSlots> src{};
        std::string warn;
        int count = 0, loaded = 0;
        for (const auto& f : job.files) if (!f.empty()) ++count;
        for (int s = 0; s < kPlayerSlots && !cancel_.load(); ++s) {
            const auto& f = job.files[static_cast<std::size_t>(s)];
            if (f.empty()) continue;
            auto r = media::buildStem(f, hc::kSampleRate, [this, loaded, count](float p) {
                progress_.store(0.3f * (static_cast<float>(loaded) + p) / static_cast<float>(std::max(1, count)));
                return !cancel_.load();
            });
            ++loaded;
            if (!r) { warn += (warn.empty() ? "" : "; ") + f.filename().string() + ": " + r.error(); continue; }
            audio[static_cast<std::size_t>(s)] = std::move(*r);
            auto& a = *audio[static_cast<std::size_t>(s)];
            src[static_cast<std::size_t>(s)] = {a.left.data(), a.right.data(), a.frames};
        }
        std::expected<ExportResult, std::string> r = cancel_.load() ? std::expected<ExportResult, std::string>(std::unexpected(std::string("Export cancelled")))
            : renderExport(job, src, [this](float p) { progress_.store(0.3f + 0.7f * p); return !cancel_.load(); });
        if (r && !warn.empty()) r->warning = r->warning.empty() ? warn : r->warning + "; " + warn;
        result_ = std::move(r);
        progress_.store(-1.0f);
        done_.store(true);
        busy_.store(false);
    });
    return true;
}

bool ConsoleExporter::takeResult(std::expected<ExportResult, std::string>& out) {
    if (!done_.load() || busy_.load()) return false;
    if (thread_.joinable()) thread_.join();
    done_.store(false);
    out = std::move(result_);
    return true;
}

// =====================================================================================================
//  Session file
// =====================================================================================================
namespace {
std::string slotKey(int s) {
    if (s == kExt1) return "ext1";
    if (s == kExt2) return "ext2";
    char b[16];
    std::snprintf(b, sizeof b, "slot%02d", s + 1);
    return b;
}
std::string u8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
std::filesystem::path fromU8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
} // namespace

std::string writeSession(const SessionData& s, const std::filesystem::path& file) {
    std::ostringstream o;
    o << "[hem-digital-console]\nformat=HEM Digital Console Session\nversion=1\n[stems]\n";
    const auto dir = file.parent_path();
    for (int k = 0; k < kPlayerSlots; ++k) {
        const auto& f = s.files[static_cast<std::size_t>(k)];
        if (f.empty()) continue;
        o << slotKey(k) << ".abs=" << u8(f) << "\n";
        std::error_code ec;
        const auto rel = dir.empty() ? std::filesystem::path() : std::filesystem::relative(f, dir, ec);
        if (!ec && !rel.empty()) o << slotKey(k) << ".rel=" << u8(rel) << "\n";
    }
    o << "[app]\nlisten=" << s.listen << "\nloop=" << (s.loop ? 1 : 0) << "\n[rig]\n" << s.rigState;
    return o.str();
}

std::expected<SessionData, std::string> readSession(const std::string& text, const std::filesystem::path& file, std::vector<int>* missing) {
    if (text.find("[hem-digital-console]") == std::string::npos) return std::unexpected(std::string("Not a HEM Digital Console session"));
    SessionData s;
    std::array<std::filesystem::path, kPlayerSlots> abs{}, rel{};
    std::string section, rigText;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[') { section = line; continue; }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (section == "[rig]") { rigText += line; rigText += '\n'; continue; }
        if (section == "[app]") {
            if (k == "listen") s.listen = std::clamp(std::atoi(v.c_str()), 0, 2);
            else if (k == "loop") s.loop = v == "1";
            continue;
        }
        if (section == "[stems]") {
            for (int slot = 0; slot < kPlayerSlots; ++slot) {
                if (k == slotKey(slot) + ".abs") abs[static_cast<std::size_t>(slot)] = fromU8(v);
                if (k == slotKey(slot) + ".rel") rel[static_cast<std::size_t>(slot)] = fromU8(v);
            }
        }
    }
    for (int slot = 0; slot < kPlayerSlots; ++slot) {
        const auto i = static_cast<std::size_t>(slot);
        if (abs[i].empty() && rel[i].empty()) continue;
        std::error_code ec;
        if (!abs[i].empty() && std::filesystem::exists(abs[i], ec)) s.files[i] = abs[i];
        else if (!rel[i].empty() && std::filesystem::exists(file.parent_path() / rel[i], ec)) s.files[i] = file.parent_path() / rel[i];
        else if (missing) missing->push_back(slot);
    }
    s.rigState = std::move(rigText);
    return s;
}

} // namespace hc::console
