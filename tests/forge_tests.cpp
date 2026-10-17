// BEAT FORGE tests (ported from Hybrid Studio 2.0.9's BeatForgeTests, plus the new parts):
// composer (every genre / scale / length, key + scale, determinism, locks), song form,
// song text round trip, every sound renders, sample-exact timing with swing, zero audio-thread
// allocation, stop / tails, auditions, and Beat Forge inside MixerProcessor + the WAV export.
#include "TestHarness.hpp"

#include "hybrid/core/Config.hpp"
#include "hybrid/engine/MixerProcessor.hpp"
#include "hybrid/forge/Composer.hpp"
#include "hybrid/forge/Forge.hpp"
#include "hybrid/forge/ForgeEngine.hpp"
#include "hybrid/media/AudioFile.hpp"
#include "hybrid/media/MixExporter.hpp"
#include "hybrid/params/MixerParams.hpp"
#include "hybrid/params/ParameterTree.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace hc::test {

namespace {
using namespace hc::forge;
constexpr double        kRate  = 48000.0;
constexpr std::uint32_t kBlock = 512;

// Beat Forge alone, bound to a full parameter tree (mixer + forge).
struct Rig {
    params::ParameterTree tree;
    std::unique_ptr<ForgeEngine> engine = std::make_unique<ForgeEngine>();
    std::uint64_t pos = 0;
    int only = -1;   // -1: every track + FX; else that track alone (no FX)

    Rig() {
        params::addMixerParameters(tree);
        addForgeParameters(tree);
        tree.find(id::on)->setPlain(1.0f);
        tree.find(id::bpm)->setPlain(120.0f);
        engine->bind(tree);
        engine->prepare(kRate, kBlock);
    }
    void set(const std::string& pid, float v) { tree.find(pid)->setPlain(v); }
    void render(std::size_t len, std::vector<float>& l, std::vector<float>& r, bool playing = true, std::uint64_t* allocs = nullptr) {
        l.assign(len, 0.0f);
        r.assign(len, 0.0f);
        for (std::size_t done = 0; done < len; done += kBlock) {
            const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(kBlock, len - done));
            std::uint64_t a = 0;
            {
                AllocScope scope;
                engine->beginBlock(n);
                engine->render(0, n, pos, playing);
                engine->endBlock(n);
                a = scope.count();
            }
            if (allocs) *allocs += a;
            for (int t = 0; t < kTracks; ++t) {
                if (only >= 0 && t != only) continue;
                for (std::uint32_t i = 0; i < n; ++i) { l[done + i] += engine->track(t, 0)[i]; r[done + i] += engine->track(t, 1)[i]; }
            }
            if (only < 0)
                for (std::uint32_t i = 0; i < n; ++i) { l[done + i] += engine->fxReturn(0)[i]; r[done + i] += engine->fxReturn(1)[i]; }
            if (playing) pos += n;
        }
    }
};

Note mk(float start, float len, int pitch, int vel = 110) {
    Note n;
    n.start = start; n.length = len; n.pitch = static_cast<std::uint8_t>(pitch); n.velocity = static_cast<std::uint8_t>(vel);
    return n;
}
double rms(const std::vector<float>& x, std::size_t from = 0, std::size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    double s = 0.0;
    for (std::size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
    return to > from ? std::sqrt(s / static_cast<double>(to - from)) : 0.0;
}
float peakOf(const std::vector<float>& x) { float p = 0.0f; for (float v : x) p = std::max(p, std::fabs(v)); return p; }
bool finite(const std::vector<float>& x) { for (float v : x) if (!std::isfinite(v)) return false; return true; }

ComposeSettings settingsFor(int genre, int scale, int bars, std::uint32_t seed) {
    ComposeSettings s;
    s.genre = genre; s.scale = scale; s.bars = bars; s.key = static_cast<int>(seed % 12);
    s.density = 0.7f; s.variation = 0.6f; s.humanize = 0.3f;
    s.harmonySeed = seed;
    for (int t = 0; t < kTracks; ++t) s.trackSeed[static_cast<std::size_t>(t)] = seed * 31u + static_cast<std::uint32_t>(t);
    return s;
}
bool sameTrack(const TrackNotes& x, const TrackNotes& y) {
    if (x.count != y.count) return false;
    for (int i = 0; i < x.count; ++i) {
        const Note& p = x.notes[static_cast<std::size_t>(i)];
        const Note& q = y.notes[static_cast<std::size_t>(i)];
        if (std::fabs(p.start - q.start) > 1.0e-4f || std::fabs(p.length - q.length) > 1.0e-4f || p.pitch != q.pitch || p.velocity != q.velocity) return false;
    }
    return true;
}
bool samePattern(const Pattern& a, const Pattern& b) {
    if (a.bars != b.bars) return false;
    for (int t = 0; t < kTracks; ++t) if (!sameTrack(a.tracks[static_cast<std::size_t>(t)], b.tracks[static_cast<std::size_t>(t)])) return false;
    return true;
}
} // namespace

void testForgeComposer() {
    std::printf("[beat forge: composer + song form]\n");
    int checked = 0, bad = 0;
    for (int g = 0; g < kNumGenres; ++g)
        for (int scale : {0, 1, 2, 3, 6, 8})
            for (int bars : {1, 2, 4, 8}) {
                ComposeResult r;
                Pattern none;
                Composer::compose(settingsFor(g, scale, bars, static_cast<std::uint32_t>(g * 100 + scale * 10 + bars)), none, r);
                const Pattern& p = r.pattern;
                int total = 0;
                bool ok = p.bars == bars && r.bpm >= 60.0 && r.bpm <= 160.0 && !r.chordNames.empty() && p.tracks[bass].count > 0;
                for (int t = 0; t < kTracks; ++t)
                    for (int i = 0; i < p.tracks[static_cast<std::size_t>(t)].count; ++i) {
                        const Note& n = p.tracks[static_cast<std::size_t>(t)].notes[static_cast<std::size_t>(i)];
                        ok = ok && n.start >= 0.0f && n.start < static_cast<float>(p.lengthSteps()) && n.length > 0.0f && n.velocity >= 1;
                        if (t == bass) ok = ok && n.pitch >= 24 && n.pitch <= 60;
                        if (t == keys || t == pad) ok = ok && n.pitch >= 45 && n.pitch <= 90;
                        if (t == lead) ok = ok && n.pitch >= 55 && n.pitch <= 92;
                        ++total;
                    }
                ok = ok && total >= 8 * bars && (g == folk || g == cinematic || p.tracks[kick].count >= bars);
                // Song form: whole patterns, every section audible, hooks play everything that has notes.
                const Arrangement a = Composer::arrange(g, p, static_cast<std::uint32_t>(g * 7 + bars), 0.6f);
                std::uint8_t present = 0;
                for (int t = 0; t < kTracks; ++t) if (p.tracks[static_cast<std::size_t>(t)].count > 0) present |= static_cast<std::uint8_t>(1u << t);
                ok = ok && a.bars > 0 && a.bars <= kMaxSongBars;
                for (int b = 0; b < a.bars; ++b) {
                    const auto m = a.mask[static_cast<std::size_t>(b)];
                    if (a.section[static_cast<std::size_t>(b)] == kHook) ok = ok && (m & present) == present;
                    if (b > 0 && a.section[static_cast<std::size_t>(b)] != a.section[static_cast<std::size_t>(b - 1)] &&
                        a.section[static_cast<std::size_t>(b)] != kDrop && a.section[static_cast<std::size_t>(b - 1)] != kDrop)
                        ok = ok && b % p.bars == 0;   // sections start on bar 1 of the pattern
                }
                if (!ok) { ++bad; std::printf("  FAIL genre %d scale %d bars %d\n", g, scale, bars); }
                ++checked;
            }
    std::printf("  %d genre / scale / length combinations composed and arranged, %d bad\n", checked, bad);
    CHECK(bad == 0 && checked == kNumGenres * 6 * 4);

    // Chords and bass stay in the key; the melody stays in the scale.
    int outside = 0;
    for (int g = 0; g < kNumGenres; ++g)
        for (int scale : {0, 1, 6}) {
            const ComposeSettings s = settingsFor(g, scale, 4, static_cast<std::uint32_t>(777 + g));
            ComposeResult r;
            Pattern none;
            Composer::compose(s, none, r);
            std::set<int> allowed, melody;
            for (int st : Composer::scaleSteps(scale == 6 ? 1 : scale)) allowed.insert((s.key + st) % 12);
            allowed.insert((s.key + 11) % 12);   // raised 7th (major V in minor)
            for (int st : Composer::scaleSteps(scale)) melody.insert((s.key + st) % 12);
            for (int t : {static_cast<int>(keys), static_cast<int>(pad)})
                for (int i = 0; i < r.pattern.tracks[static_cast<std::size_t>(t)].count; ++i)
                    outside += allowed.count(r.pattern.tracks[static_cast<std::size_t>(t)].notes[static_cast<std::size_t>(i)].pitch % 12) == 0;
            for (int i = 0; i < r.pattern.tracks[lead].count; ++i) {
                const int pc = r.pattern.tracks[lead].notes[static_cast<std::size_t>(i)].pitch % 12;
                outside += melody.count(pc) == 0 && allowed.count(pc) == 0;
            }
        }
    CHECK(outside == 0);

    // Deterministic; a re-rolled track changes alone.
    const ComposeSettings s = settingsFor(trap, 1, 4, 4242);
    ComposeResult a, b, c;
    Pattern none;
    Composer::compose(s, none, a);
    Composer::compose(s, none, b);
    CHECK(samePattern(a.pattern, b.pattern));
    ComposeSettings s2 = s;
    s2.trackSeed[lead] += 1;
    for (int t = 0; t < kTracks; ++t) s2.locked[static_cast<std::size_t>(t)] = t != lead;
    Composer::compose(s2, a.pattern, c);
    bool othersSame = true;
    for (int t = 0; t < kTracks; ++t)
        if (t != lead) othersSame = othersSame && sameTrack(a.pattern.tracks[static_cast<std::size_t>(t)], c.pattern.tracks[static_cast<std::size_t>(t)]);
    CHECK(othersSame && !sameTrack(a.pattern.tracks[lead], c.pattern.tracks[lead]));

    // Trap song form, as text.
    const Arrangement form = Composer::arrange(trap, a.pattern, 4242, 0.6f);
    const Arrangement form2 = Composer::arrange(trap, a.pattern, 4242, 0.6f);
    CHECK(form.bars == form2.bars && form.mask == form2.mask);
    std::printf("  trap song form at %.0f BPM: %s (%d bars = %.0f s)\n", a.bpm, Composer::describe(form).c_str(), form.bars,
                form.bars * 4 * 60.0 / a.bpm);
    std::printf("  chords: %s\n", a.chordNames.c_str());
    CHECK(form.bars >= 64 && form.section[0] == kIntro && form.section[static_cast<std::size_t>(form.bars - 1)] == kOutro);

    // Song text round trip (sessions, export).
    Song song{a.pattern, form};
    Song back;
    CHECK(songFromText(songToText(song), back) && samePattern(back.pattern, song.pattern) && back.arrangement.bars == form.bars &&
          back.arrangement.mask == form.mask && back.arrangement.section == form.section);
    Song junk;
    CHECK(!songFromText("hello\nkick=x,y;;\n", junk) || junk.pattern.isEmpty());
}

void testForgeEngine() {
    std::printf("[beat forge: sounds, timing, real-time safety]\n");
    // Every drum sound and every melodic sound type renders, bounded and finite.
    const int drumNotes[4][8] = {{36, 31, 43, 36, 36, 36, 36, 36}, {38, 39, 37, 40, 38, 38, 38, 38},
                                 {42, 44, 46, 42, 42, 42, 42, 42}, {45, 47, 50, 49, 54, 56, 70, 75}};
    int silent = 0, loud = 0, nan = 0, sounds = 0;
    for (int t = 0; t < kTracks; ++t) {
        const int variants = isDrum(t) ? 8 : kTypes;
        for (int v = 0; v < variants; ++v) {
            Rig rig;
            rig.only = t;
            if (!isDrum(t)) rig.set(id::type(t), static_cast<float>(v));
            Song s;
            s.pattern.bars = 1;
            const int pitch = isDrum(t) ? drumNotes[t][v] : (t == bass ? 36 : 60);
            s.pattern.tracks[static_cast<std::size_t>(t)].add(mk(0, 4, pitch));
            rig.engine->setSong(s);
            std::vector<float> l, r;
            rig.render(static_cast<std::size_t>(kRate * 0.6), l, r);
            const double level = std::max(rms(l, 0, static_cast<std::size_t>(kRate * 0.15)), rms(r, 0, static_cast<std::size_t>(kRate * 0.15)));
            const float pk = std::max(peakOf(l), peakOf(r));
            nan += !(finite(l) && finite(r));
            silent += isDrum(t) ? pk <= 0.02f : level <= 0.001;
            loud += pk >= 2.5f;
            ++sounds;
        }
    }
    std::printf("  %d sounds rendered: %d silent, %d too loud, %d not finite\n", sounds, silent, loud, nan);
    CHECK(silent == 0 && loud == 0 && nan == 0 && sounds == 4 * 8 + 4 * kTypes);

    // Notes start on the exact sample; swing delays odd 16ths.
    const double stepLen = ForgeEngine::stepSamples(kRate, 120.0);   // 6000 samples
    auto renderKick = [](float swing, bool withStep1) {
        Rig rig;
        rig.only = kick;
        rig.set(id::knob(kick, 3), 0.0f);   // no click noise: the onset is the sine start
        rig.set(id::swing, swing);
        Song s;
        s.pattern.bars = 1;
        s.pattern.tracks[kick].add(mk(0, 1, 36));
        s.pattern.tracks[kick].add(mk(4, 1, 36));
        if (withStep1) s.pattern.tracks[kick].add(mk(1, 1, 36));
        s.pattern.tracks[kick].sort();
        rig.engine->setSong(s);
        std::vector<float> l, r;
        rig.render(30000, l, r);
        return l;
    };
    int timingBad = 0;
    for (float sw : {50.0f, 66.0f, 75.0f}) {
        const auto a = renderKick(sw, false), b = renderKick(sw, true);
        int first = -1, diff = -1;
        for (int i = 0; i < static_cast<int>(a.size()); ++i) if (std::fabs(a[static_cast<std::size_t>(i)]) > 1.0e-7f) { first = i; break; }
        for (int i = 0; i < static_cast<int>(a.size()); ++i)
            if (std::fabs(a[static_cast<std::size_t>(i)] - b[static_cast<std::size_t>(i)]) > 1.0e-7f) { diff = i; break; }
        const int expected1 = static_cast<int>(std::floor(stepLen * (1.0 + (sw / 50.0 - 1.0))));
        const int e4 = static_cast<int>(4 * stepLen);
        double before = 0.0, after = 0.0;
        for (int i = e4 - 400; i < e4; ++i) before = std::max(before, static_cast<double>(std::fabs(a[static_cast<std::size_t>(i)])));
        for (int i = e4; i < e4 + 400; ++i) after = std::max(after, static_cast<double>(std::fabs(a[static_cast<std::size_t>(i)])));
        const bool ok = (first == 0 || first == 1) && (diff == expected1 || diff == expected1 + 1) && after > 3.0 * before;
        if (!ok) std::printf("  swing %.0f: step 0 at %d, step 1 at %d (expected %d)\n", sw, first, diff, expected1);
        timingBad += !ok;
    }
    std::printf("  kick on the exact sample at swing 50 / 66 / 75 %%: %s\n", timingBad ? "NO" : "yes");
    CHECK(timingBad == 0);

    // Zero allocations: full 8-bar gospel song, both effects, parameter moves, hand-played notes, stop / start.
    {
        Rig rig;
        ComposeResult r;
        Pattern none;
        ComposeSettings s = settingsFor(gospel, 0, 8, 99);
        s.density = 1.0f; s.variation = 1.0f;
        Composer::compose(s, none, r);
        rig.engine->setSong(Song{r.pattern, Composer::arrange(gospel, r.pattern, 99, 1.0f)});
        for (int t = 0; t < kTracks; ++t) { rig.set(id::rev(t), 40.0f); rig.set(id::dly(t), 30.0f); }
        std::uint64_t allocs = 0;
        std::vector<float> l, rr;
        for (int round = 0; round < 12; ++round) {
            rig.set(id::knob(round % kTracks, round % kKnobs), static_cast<float>(round * 7 % 100));
            if (round % 3 == 0) rig.set(id::type(keys), static_cast<float>(round % 4));
            rig.engine->audition(round % kTracks, 60 + round, 100);
            rig.render(static_cast<std::size_t>(kRate / 2), l, rr, round != 7, &allocs);
        }
        std::printf("  6 s of an 8-bar gospel song with effects, knob moves, stop / start and hand-played notes: %llu heap allocations\n",
                    static_cast<unsigned long long>(allocs));
        CHECK(allocs == 0 && finite(l));
    }

    // Many overlapping notes, slides, re-hits and more notes than voices: finite and bounded.
    {
        int badCount = 0;
        for (int t : {static_cast<int>(bass), static_cast<int>(keys), static_cast<int>(lead), static_cast<int>(pad)})
            for (int ty = 0; ty < kTypes; ++ty) {
                Rig rig;
                rig.only = t;
                rig.set(id::type(t), static_cast<float>(ty));
                Song s;
                s.pattern.bars = 2;
                const int base = t == bass ? 36 : 60;
                for (int k = 0; k < 32; ++k) {
                    const float len = (k % 4 == 3) ? 1.4f : (k % 2 == 0 ? 0.6f : 3.0f);
                    s.pattern.tracks[static_cast<std::size_t>(t)].add(mk(static_cast<float>(k), len, base + (k * 5) % 12, 90 + (k % 3) * 10));
                    if (t == keys || t == pad)
                        for (int c = 1; c < 4; ++c) s.pattern.tracks[static_cast<std::size_t>(t)].add(mk(static_cast<float>(k), len, base + (k * 5) % 12 + 3 * c, 80));
                }
                s.pattern.tracks[static_cast<std::size_t>(t)].sort();
                rig.engine->setSong(s);
                std::vector<float> l, r;
                rig.render(static_cast<std::size_t>(kRate * 4), l, r);
                badCount += !(finite(l) && finite(r) && peakOf(l) < 3.0f && peakOf(r) < 3.0f && rms(l) > 1e-3);
            }
        CHECK(badCount == 0);
    }

    // Stop releases held notes and the tails die away; a hand-played note sounds while stopped.
    {
        Rig rig;
        Song s;
        s.pattern.bars = 1;
        for (int t = 0; t < kTracks; ++t) s.pattern.tracks[static_cast<std::size_t>(t)].add(mk(0, 16, isDrum(t) ? 36 + 2 * t : 48 + 5 * t));
        rig.engine->setSong(s);
        std::vector<float> l, r;
        rig.render(static_cast<std::size_t>(kRate), l, r, true);
        const double playing = rms(l);
        rig.render(static_cast<std::size_t>(kRate * 8), l, r, false);
        const double tail = std::max(rms(l, static_cast<std::size_t>(kRate * 7)), rms(r, static_cast<std::size_t>(kRate * 7)));
        std::printf("  every track held, then stop: %.1f dB 7 s later\n", 20.0 * std::log10(std::max(tail, 1e-10)));
        CHECK(playing > 0.01 && tail < 1.0e-3);
        Rig a;
        a.engine->audition(keys, 64, 110);
        a.render(12000, l, r, false);
        CHECK(rms(l) > 0.003);
    }
    {   // Stereo spread: kick and 808 stay centred, hats / keys / pad go wide, and the whole beat
        // keeps its level in mono (the side signal cancels in L+R instead of eating the centre).
        ComposeSettings cs = settingsFor(trap, 1, 4, 21);
        ComposeResult res;
        Composer::compose(cs, Pattern{}, res);
        Song song;
        song.pattern = res.pattern;
        auto corrOf = [&](int only, double& monoDropDb) {
            Rig rig;
            rig.only = only;
            for (int t = bass; t < kTracks; ++t) rig.set(id::type(t), static_cast<float>(res.soundType[static_cast<std::size_t>(t)]));
            rig.engine->setSong(song);
            std::vector<float> l, r;
            rig.render(static_cast<std::size_t>(kRate * 8), l, r);
            double lr = 0, ll = 0, rr = 0, mono = 0;
            for (std::size_t i = 0; i < l.size(); ++i) {
                lr += static_cast<double>(l[i]) * r[i]; ll += static_cast<double>(l[i]) * l[i]; rr += static_cast<double>(r[i]) * r[i];
                const double m = 0.5 * (static_cast<double>(l[i]) + r[i]);
                mono += m * m;
            }
            monoDropDb = 10.0 * std::log10(std::max(1e-30, mono) / std::max(1e-30, 0.5 * (ll + rr)));
            return ll > 0 && rr > 0 ? lr / std::sqrt(ll * rr) : 1.0;
        };
        double drop = 0, dropMix = 0;
        const double cKick = corrOf(kick, drop), cBass = corrOf(bass, drop), cHats = corrOf(hats, drop), cKeys = corrOf(keys, drop),
                     cPad = corrOf(pad, drop), cMix = corrOf(-1, dropMix);
        std::printf("  stereo (1 = mono): kick %.3f, 808 %.3f, hats %.2f, keys %.2f, pad %.2f, whole beat %.2f; whole beat in mono %.2f dB\n", cKick, cBass,
                    cHats, cKeys, cPad, cMix, dropMix);
        CHECK(cKick > 0.999 && cBass > 0.999);
        CHECK(cHats < 0.85 && cKeys < 0.85 && cPad < 0.85);
        CHECK(cMix < 0.97 && dropMix > -1.0);
    }
    {   // RESET: mixer and Beat Forge sound knobs go back to their defaults; the beat (on, tempo,
        // genre, key, form, sound types, locks) stays; the saved text puts everything back (UNDO).
        params::ParameterTree tree;
        params::addMixerParameters(tree);
        addForgeParameters(tree);
        auto* trim = tree.find(params::stemParamId(0, "trim"));
        auto* mainFader = tree.find(params::stemParamId(0, "main"));
        auto* kickDecay = tree.find(id::knob(kick, 1));
        auto* keysRev = tree.find(id::rev(keys));
        trim->setPlain(6.0f);
        mainFader->setPlain(1.5f);
        kickDecay->setPlain(1200.0f);
        keysRev->setPlain(80.0f);
        tree.find(id::on)->setPlain(1.0f);
        tree.find(id::bpm)->setPlain(150.0f);
        tree.find(id::genre)->setPlain(3.0f);
        tree.find(id::type(bass))->setPlain(2.0f);
        tree.find(id::lock(snare))->setPlain(1.0f);
        const std::string before = tree.serialize();
        const std::size_t n = tree.resetToDefaults([](const std::string& pid) { return keepOnReset(pid); });
        std::printf("  RESET: %zu controls back to default; beat settings kept\n", n);
        CHECK(n == 4);
        CHECK(trim->plain() == trim->spec().def && mainFader->plain() == mainFader->spec().def && kickDecay->plain() == kickDecay->spec().def &&
              keysRev->plain() == keysRev->spec().def);
        CHECK(tree.find(id::on)->isOn() && tree.find(id::bpm)->plain() == 150.0f && tree.find(id::genre)->plain() == 3.0f &&
              tree.find(id::type(bass))->plain() == 2.0f && tree.find(id::lock(snare))->isOn());
        tree.deserialize(before);
        CHECK(trim->plain() == 6.0f && mainFader->plain() == 1.5f && kickDecay->plain() == 1200.0f && keysRev->plain() == 80.0f);
        CHECK(!keepOnReset("stem01.trim") && !keepOnReset("forge.kick.decay") && keepOnReset("forge.pad.type") && keepOnReset("forge.genre"));
        CHECK(tree.resetToDefaults() > 0 && !tree.find(id::on)->isOn());   // no keep rule: everything
    }
}

void testForgeInMixer() {
    std::printf("[beat forge in the console]\n");
    params::ParameterTree tree;
    params::addMixerParameters(tree);
    addForgeParameters(tree);
    const params::MixerBindings bindings(tree);
    auto mp = std::make_unique<engine::MixerProcessor>(engine::MixerProcessor::Source::Stems);
    io::StreamConfig cfg;
    cfg.sampleRate = 48000; cfg.blockFrames = 480; cfg.numOutputs = 2;
    mp->prepare(cfg);
    params::bindMixerParameters(tree, bindings, mp->matrix(), &mp->limiter());
    tree.dispatchChanges();
    mp->forge().bind(tree);

    ComposeResult r;
    Pattern none;
    Composer::compose(settingsFor(trap, 1, 4, 2026), none, r);
    const Song song{r.pattern, Composer::arrange(trap, r.pattern, 2026, 0.5f)};
    mp->forge().setSong(song);
    tree.find(id::on)->setPlain(1.0f);
    tree.find(id::bpm)->setPlain(static_cast<float>(r.bpm));

    std::vector<float> out(2 * 480);
    float* outs[2] = {out.data(), out.data() + 480};
    engine::ProcessContext ctx;
    ctx.outputs = outs; ctx.numOutputs = 2; ctx.frames = 480;
    mp->play();
    double sumSq = 0.0;
    std::uint64_t allocs = 0;
    const auto tc = std::chrono::steady_clock::now();
    {
        AllocScope scope;
        for (int b = 0; b < 1000; ++b) {   // 10 s
            mp->process(ctx);
            for (float v : out) sumSq += static_cast<double>(v) * v;
        }
        allocs = scope.count();
    }
    const double usPerBlock = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tc).count() / 1000.0;
    std::printf("  CPU: %.0f us per 480-frame block = %.1f %% of one core (CI machine; Beat Forge + 13 strips + limiter)\n", usPerBlock,
                100.0 * usPerBlock / 10000.0);
    const double expectFrames = static_cast<double>(song.arrangement.bars) * 16.0 * ForgeEngine::stepSamples(48000.0, r.bpm);
    std::printf("  trap song through strips 1-8 + limiter: song %.1f s (%d bars), 10 s played at %.1f dBFS RMS, kick strip peak %.3f, %llu heap allocations\n",
                expectFrames / 48000.0, song.arrangement.bars, 10.0 * std::log10(std::max(sumSq / (1000.0 * 960.0), 1e-20)),
                static_cast<double>(mp->matrix().stemPeak(0)), static_cast<unsigned long long>(allocs));
    CHECK(mp->forgeActive() && allocs == 0 && sumSq > 0.0 && std::llabs(static_cast<long long>(mp->stemSongFrames()) - static_cast<long long>(expectFrames)) <= 1);

    // The arrangement: the intro has no kick, so strip 1 stays silent until the first section with drums.
    {
        auto m2 = std::make_unique<engine::MixerProcessor>(engine::MixerProcessor::Source::Stems);
        m2->prepare(cfg);
        m2->forge().bind(tree);
        m2->forge().setSong(song);
        m2->play();
        float kickInIntro = 0.0f;
        const auto introFrames = static_cast<std::uint64_t>(4.0 * 16.0 * ForgeEngine::stepSamples(48000.0, r.bpm));   // first 4 bars
        for (std::uint64_t f = 0; f + 480 <= introFrames; f += 480) { m2->process(ctx); kickInIntro = std::max(kickInIntro, m2->matrix().stemPeak(0)); }
        CHECK(song.arrangement.section[0] == kIntro && ((song.arrangement.mask[0] >> kick) & 1u) == 0 && kickInIntro == 0.0f);
    }

    // Export: the WAV holds the Beat Forge song plus 2 s of tails, deterministic.
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "hc_forge_export";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    media::ExportRequest req;
    req.params = tree.serialize();
    req.forgeSong = songToText(song);
    req.out = dir / "beat.wav";
    const auto t0 = std::chrono::steady_clock::now();
    auto res = media::renderMix(req);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(res.has_value());
    if (res) {
        const auto want = static_cast<std::uint64_t>(std::llround(expectFrames)) + 2 * 48000;
        std::printf("  exported %.1f s of Beat Forge (24-bit) in %.2f s, peak %.3f\n", static_cast<double>(res->frames) / 48000.0, secs,
                    static_cast<double>(res->peak));
        CHECK(res->frames == want && res->peak > 0.05f && res->peak <= 0.9661f && res->clipped == 0);
        std::ifstream a(req.out, std::ios::binary);
        const std::vector<char> first{std::istreambuf_iterator<char>(a), std::istreambuf_iterator<char>()};
        a.close();
        auto again = media::renderMix(req);
        std::ifstream b(req.out, std::ios::binary);
        const std::vector<char> second{std::istreambuf_iterator<char>(b), std::istreambuf_iterator<char>()};
        CHECK(again && first == second);
    }
    fs::remove_all(dir, ec);
}

} // namespace hc::test
