#pragma once
// =============================================================================
//  Beat Forge composer (ported from Hybrid Studio 2.0.9, JUCE-free): genre templates for
//  drums, diatonic chords with voice leading, bass that locks to the kick, motif melodies.
//  New here: arrange() turns the pattern into a full song form (intro, verses, hooks,
//  bridge, drop, outro) by choosing which tracks play in each bar.
//  Pure functions of their settings (message / worker thread; they allocate freely).
// =============================================================================
#include "hybrid/forge/Pattern.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hc::forge
{
enum Genre { trap = 0, popTrap, drill, boomBap, outlawCountry, folk, gospel, cinematic, kNumGenres };

/** Deterministic random numbers (identical on every platform / compiler, unlike <random> distributions). */
struct Rng
{
    uint64_t s = 0x9E3779B97F4A7C15ull;
    explicit Rng (uint64_t seed) : s (seed ^ 0x9E3779B97F4A7C15ull) { next(); next(); }
    uint64_t next() noexcept
    {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double unit() noexcept { return (double) (next() >> 11) * (1.0 / 9007199254740992.0); }   // [0, 1)
    int below (int n) noexcept { return n <= 1 ? 0 : (int) (unit() * n); }
    bool chance (double p) noexcept { return unit() < p; }
    double range (double lo, double hi) noexcept { return lo + (hi - lo) * unit(); }
};

struct ComposeSettings
{
    int genre = trap;
    int key = 9;            // 0 = C .. 11 = B
    int scale = 1;          // index into scaleChoices()
    int bars = 4;           // 1, 2, 4 or 8
    float density = 0.55f, variation = 0.4f, humanize = 0.3f;   // 0..1
    uint32_t harmonySeed = 1;                                    // chords (shared by every track)
    std::array<uint32_t, kTracks> trackSeed {};                  // each track's own part
    std::array<bool, kTracks> locked {};                          // locked tracks keep the current pattern's notes
};

struct ComposeResult
{
    Pattern pattern;
    double bpm = 120.0;
    std::array<int, kTracks> soundType {};    // suggested sound type per melodic track (index into typeChoices)
    std::string chordNames;                  // e.g. "Am | F | C | G"
};

/** Rule-based composer: genre templates for drums, chords with voice leading, bass lines and
    motif-based melodies. Pure function of its settings (message / worker thread; allocates freely). */
class Composer
{
public:
    static void compose (const ComposeSettings&, const Pattern& current, ComposeResult& out);

    /** Typical tempo for a genre (picked deterministically from the harmony seed). */
    static double genreTempo (int genre, uint32_t seed);
    static double genreSwing (int genre);   // transport swing % that suits the genre (50 = straight)

    /** Scale intervals in semitones (5..7 entries). */
    static const std::vector<int>& scaleSteps (int scaleIndex);

    /** Song form for this genre: which tracks play in each bar (tracks without notes are ignored).
        Sections are whole multiples of the pattern length, so every section starts on bar 1 of the
        pattern. Deterministic for the same inputs. */
    static Arrangement arrange (int genre, const Pattern& pattern, uint32_t seed, float variation);
    /** "INTRO 8 · HOOK 16 · VERSE 16 ..." */
    static std::string describe (const Arrangement&);
};
} // namespace hc::forge
