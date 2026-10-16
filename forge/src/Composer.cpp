#include "hybrid/forge/Composer.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace hc::forge
{
namespace
{
//==============================================================================
// Harmony
//==============================================================================
const std::vector<std::vector<int>>& scaleTable()
{
    static const std::vector<std::vector<int>> t {
        { 0, 2, 4, 5, 7, 9, 11 },   // Major
        { 0, 2, 3, 5, 7, 8, 10 },   // Minor
        { 0, 2, 3, 5, 7, 8, 11 },   // Harmonic Minor
        { 0, 2, 3, 5, 7, 9, 10 },   // Dorian
        { 0, 2, 4, 5, 7, 9, 10 },   // Mixolydian
        { 0, 1, 3, 5, 7, 8, 10 },   // Phrygian
        { 0, 3, 5, 7, 10 },         // Minor Pentatonic
        { 0, 2, 4, 7, 9 },          // Major Pentatonic
        { 0, 3, 5, 6, 7, 10 },      // Blues
    };
    return t;
}

/** 7-note scale the chords are built from (pentatonic / blues borrow their parent). */
const std::vector<int>& parentScale (int scale)
{
    const auto& t = scaleTable();
    switch (scale)
    {
        case 6: case 8: return t[1];
        case 7:         return t[0];
        default:        return t[(size_t) std::clamp (scale, 0, 5)];
    }
}

bool minorish (int scale) { return parentScale (scale)[2] == 3; }

/** Note name spelled for the key: sharp keys (G D A E B F# C# and their relative minors) use sharps, the rest flats. */
const char* pcName (int pc, int key, bool minor)
{
    static constexpr const char* sharp[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static constexpr const char* flat[12]  = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    const int major = ((minor ? key + 3 : key) % 12 + 12) % 12;
    const bool useSharps = major == 0 || major == 7 || major == 2 || major == 9 || major == 4 || major == 11 || major == 6 || major == 1;
    return (useSharps ? sharp : flat)[((pc % 12) + 12) % 12];
}

struct Chord
{
    int root = 0;                 // pitch class (0..11), absolute
    std::vector<int> tones;       // semitones above the root, ascending, first = 0
    float start = 0, length = 16; // steps
    std::string name;
};

/** Diatonic chord on `degree` of the parent scale, optional 7th / 9th, optional raised (major) third on V. */
Chord diatonic (int key, int scale, int degree, bool seventh, bool ninth, bool majorV, bool sus2)
{
    const auto& P = parentScale (scale);
    auto at = [&P] (int d) { return P[(size_t) (d % 7)] + 12 * (d / 7); };
    Chord c;
    const int r = at (degree);
    c.root = (key + r) % 12;
    int third = at (degree + 2) - r, fifth = at (degree + 4) - r;
    if (majorV && degree % 7 == 4) third = 4;
    if (sus2) third = 2;
    c.tones = { 0, third, fifth };
    int sev = -1;
    if (seventh) { sev = at (degree + 6) - r; if (majorV && degree % 7 == 4) sev = 10; c.tones.push_back (sev); }
    if (ninth && (at (degree + 8) - r) % 12 != 1) c.tones.push_back (at (degree + 8) - r);   // never a b9
    else ninth = false;

    std::string q;
    const bool dim = third == 3 && fifth == 6;
    if (sus2) q = "sus2";
    else if (dim) q = sev == 9 ? "dim7" : (sev == 10 ? "m7b5" : "dim");
    else if (third == 3) q = seventh ? "m7" : "m";
    else q = seventh ? (sev == 11 ? "maj7" : "7") : "";
    if (ninth && ! sus2) q += "(9)";
    c.name = std::string (pcName (c.root, key, minorish (scale))) + q;
    return c;
}

struct GenreInfo
{
    std::vector<std::array<int, 4>> major, minor;
    bool sevenths, majorV;
};

const GenreInfo& genreInfo (int g)
{
    static const std::array<GenreInfo, kNumGenres> info {{
        { { { 5, 3, 0, 4 }, { 0, 4, 5, 3 }, { 0, 5, 3, 4 } },                              // Trap
          { { 0, 5, 2, 6 }, { 0, 5, 3, 4 }, { 0, 3, 5, 4 }, { 0, 6, 5, 6 }, { 0, 0, 5, 6 }, { 0, 2, 6, 5 } }, false, false },
        { { { 0, 4, 5, 3 }, { 5, 3, 0, 4 }, { 0, 5, 3, 4 }, { 3, 4, 5, 5 } },              // Pop Trap
          { { 0, 5, 2, 6 }, { 0, 2, 6, 5 }, { 5, 6, 0, 0 }, { 0, 3, 5, 6 } }, false, false },
        { { { 0, 5, 3, 4 }, { 5, 3, 4, 4 } },                                              // Drill
          { { 0, 0, 5, 4 }, { 0, 5, 0, 4 }, { 0, 3, 5, 4 }, { 0, 6, 5, 4 } }, false, true },
        { { { 1, 4, 0, 5 }, { 0, 5, 1, 4 }, { 3, 2, 1, 0 } },                              // Boom Bap
          { { 0, 3, 0, 3 }, { 1, 4, 0, 0 }, { 0, 5, 1, 4 }, { 0, 3, 6, 2 } }, true, false },
        { { { 0, 3, 4, 0 }, { 0, 0, 3, 4 }, { 0, 4, 5, 3 }, { 0, 3, 0, 4 } },              // Outlaw Country
          { { 0, 5, 6, 0 }, { 0, 3, 4, 0 }, { 0, 6, 3, 0 } }, false, true },
        { { { 0, 3, 0, 4 }, { 5, 3, 0, 4 }, { 0, 5, 3, 4 }, { 0, 4, 5, 3 } },              // Folk
          { { 0, 6, 5, 6 }, { 0, 5, 6, 0 }, { 0, 3, 0, 4 } }, false, true },
        { { { 0, 3, 1, 4 }, { 0, 5, 1, 4 }, { 3, 4, 2, 5 }, { 0, 2, 3, 4 } },              // Gospel
          { { 0, 3, 6, 2 }, { 1, 4, 0, 5 }, { 0, 5, 1, 4 } }, true, true },
        { { { 0, 4, 5, 3 }, { 3, 0, 4, 5 }, { 5, 3, 0, 4 } },                              // Cinematic
          { { 0, 5, 2, 6 }, { 0, 5, 3, 4 }, { 0, 3, 0, 5 }, { 5, 3, 0, 4 } }, false, true },
    }};
    return info[(size_t) std::clamp (g, 0, kNumGenres - 1)];
}

std::vector<Chord> buildChords (const ComposeSettings& s, Rng& rng)
{
    const auto& gi = genreInfo (s.genre);
    const bool minor = minorish (s.scale);
    const auto& bank = minor ? gi.minor : gi.major;
    const auto& prog = bank[(size_t) rng.below ((int) bank.size())];
    const auto& prog2 = bank[(size_t) rng.below ((int) bank.size())];   // second half of an 8-bar phrase

    std::vector<int> degrees;
    std::vector<float> lengths;
    switch (s.bars)
    {
        case 1:  degrees = { prog[0], prog[1] }; lengths = { 8, 8 }; break;
        case 2:  degrees = { prog[0], prog[1], prog[2], prog[3] }; lengths = { 8, 8, 8, 8 }; break;
        case 8:  degrees = { prog[0], prog[1], prog[2], prog[3], prog2[0], prog2[1], prog2[2], prog2[3] }; lengths.assign (8, 16.0f); break;
        default: degrees = { prog[0], prog[1], prog[2], prog[3] }; lengths.assign (4, 16.0f); break;
    }
    std::vector<Chord> out;
    float t = 0;
    for (size_t i = 0; i < degrees.size(); ++i)
    {
        const bool seventh = gi.sevenths || (s.genre == popTrap && rng.chance (0.3 * s.variation));
        const bool ninth = (s.genre == gospel || s.genre == boomBap) && rng.chance (0.25 + 0.5 * s.variation);
        const bool sus2 = s.genre == cinematic && rng.chance (0.3 * s.variation) && degrees[i] != 4;
        auto c = diatonic (s.key, s.scale, degrees[i], seventh, ninth, gi.majorV && minor, sus2);
        c.start = t; c.length = lengths[i];
        t += lengths[i];
        out.push_back (c);
    }
    return out;
}

const Chord& chordAt (const std::vector<Chord>& chords, float step)
{
    for (auto& c : chords)
        if (step >= c.start && step < c.start + c.length) return c;
    return chords.back();
}

/** Pitch of a chord tone (index into tones, wraps up octaves) nearest to `near`. */
int chordToneNear (const Chord& c, int idx, int near)
{
    const int n = (int) c.tones.size();
    const int semis = c.tones[(size_t) (idx % n)] + 12 * (idx / n);
    int p = c.root + semis;
    while (p + 12 <= near + 6) p += 12;
    while (p - 12 >= near - 6) p -= 12;
    while (p < near - 6) p += 12;
    return p;
}

/** Voicing with the smallest total movement from `prev` (or nearest to `centre`). */
std::vector<int> voiceChord (const Chord& c, const std::vector<int>& prev, int lo, int hi, int centre, int maxNotes)
{
    const int n = std::min ((int) c.tones.size(), maxNotes);
    std::vector<int> best;
    double bestCost = 1.0e9;
    for (int inv = 0; inv < n; ++inv)
        for (int baseOct = lo / 12 - 1; baseOct <= hi / 12 + 1; ++baseOct)
        {
            std::vector<int> v;
            int last = -100;
            for (int k = 0; k < n; ++k)
            {
                int p = 12 * baseOct + c.root + c.tones[(size_t) ((inv + k) % n)];
                while (p <= last) p += 12;
                v.push_back (p);
                last = p;
            }
            if (v.front() < lo || v.back() > hi) continue;
            double cost = 0.0;
            if (prev.size() == v.size())
                for (size_t k = 0; k < v.size(); ++k) cost += std::abs (v[k] - prev[k]);
            else
            {
                double mean = 0.0;
                for (int p : v) mean += p;
                cost = std::abs (mean / (double) v.size() - centre) * 2.0;
            }
            if (cost < bestCost) { bestCost = cost; best = v; }
        }
    if (best.empty())
        for (int k = 0; k < n; ++k) best.push_back (std::clamp (centre + c.tones[(size_t) k], lo, hi));
    return best;
}

//==============================================================================
// Note helpers
//==============================================================================
struct Writer
{
    TrackNotes& tn;
    const ComposeSettings& s;
    Rng& rng;
    int track;
    int steps;

    void note (float start, float length, int pitch, int velocity)
    {
        if (start < 0.0f || start >= (float) steps) return;
        Note n;
        const float h = s.humanize;
        const float jitter = isDrum (track) ? (track == hats ? 0.04f : 0.02f) : 0.07f;
        n.start = std::clamp (start + (start > 0.05f ? (float) (rng.unit() * 2.0 - 1.0) * h * jitter : 0.0f), 0.0f, (float) steps - 0.01f);
        n.length = std::max (0.1f, std::min (length, (float) steps - n.start));
        n.pitch = (unsigned char) std::clamp (pitch, 0, 127);
        const double v = velocity * (1.0 + (rng.unit() * 2.0 - 1.0) * 0.18 * h);
        n.velocity = (unsigned char) std::clamp ((int) std::lround (v), 1, 127);
        tn.add (n);
    }
};

//==============================================================================
// Drums
//==============================================================================
void drumsFor (const ComposeSettings& s, Pattern& pat, Rng& rngK, Rng& rngS, Rng& rngH, Rng& rngP, const std::array<bool, kTracks>& write)
{
    const int bars = s.bars, steps = bars * 16;
    const float d = s.density, v = s.variation;
    Writer K { pat.tracks[kick], s, rngK, kick, steps }, S { pat.tracks[snare], s, rngS, snare, steps };
    Writer H { pat.tracks[hats], s, rngH, hats, steps }, P { pat.tracks[perc], s, rngP, perc, steps };

    using Pat = std::vector<int>;
    auto pick = [] (Rng& r, const std::vector<Pat>& opts) -> const Pat& { return opts[(size_t) r.below ((int) opts.size())]; };

    std::vector<Pat> kicks;
    switch (s.genre)
    {
        case trap:          kicks = { { 0, 10 }, { 0, 7, 10 }, { 0, 3, 10 }, { 0, 6, 10, 14 }, { 0, 10, 11 }, { 0, 7, 11, 14 } }; break;
        case popTrap:       kicks = { { 0, 10 }, { 0, 6, 10 }, { 0, 3, 6, 10 }, { 0, 10, 14 } }; break;
        case drill:         kicks = { { 0, 11 }, { 0, 7 }, { 0, 10 }, { 0, 6, 11 } }; break;
        case boomBap:       kicks = { { 0, 10 }, { 0, 7, 10 }, { 0, 3, 10 }, { 0, 9, 10 } }; break;
        case outlawCountry: kicks = { { 0, 8 }, { 0, 7, 8 }, { 0, 8, 10 } }; break;
        case folk:          kicks = { { 0 }, { 0, 8 }, { 0, 10 } }; break;
        case gospel:        kicks = { { 0, 10 }, { 0, 7, 10 }, { 0, 3, 8, 10 } }; break;
        default:            kicks = { { 0 }, { 0, 10 }, { 0, 6 } }; break;
    }
    const Pat& kickA = pick (rngK, kicks);
    const Pat& kickB = pick (rngK, kicks);

    for (int b = 0; b < bars; ++b)
    {
        const int o = b * 16;
        const bool lastBar = b == bars - 1 && bars >= 2;
        const bool phraseStart = b == 0;

        // ---- kick ----
        if (write[kick] && ! (s.genre == folk && d < 0.25f) && ! (s.genre == cinematic && d < 0.2f))
        {
            const Pat& kp = (b % 2 == 1 && rngK.chance (0.6 * v)) ? kickB : kickA;
            for (int st : kp) K.note ((float) (o + st), 1.0f, drum::kick, st == 0 ? 120 : 105);
            if (lastBar && rngK.chance (0.5 * v + 0.2 * d)) K.note ((float) (o + 14 + rngK.below (2)), 1.0f, drum::kick, 95);
        }

        // ---- snare / clap ----
        if (write[snare])
        {
            switch (s.genre)
            {
                case trap: case popTrap:
                    S.note ((float) (o + 8), 1.0f, s.genre == trap ? drum::snare2 : drum::clap, 115);
                    if (rngS.chance (0.25 * v)) S.note ((float) (o + 15), 1.0f, drum::snare, 70);
                    break;
                case drill:
                {
                    S.note ((float) (o + 8), 1.0f, drum::snare2, 115);
                    static const int extra[] = { 13, 14, 11, 3 };
                    if (rngS.chance (0.4 + 0.4 * v)) S.note ((float) (o + extra[rngS.below (4)]), 1.0f, drum::snare, 95);
                    break;
                }
                case boomBap: case gospel: case outlawCountry:
                    S.note ((float) (o + 4), 1.0f, drum::snare, s.genre == outlawCountry ? 95 : 112);
                    S.note ((float) (o + 12), 1.0f, drum::snare, s.genre == outlawCountry ? 95 : 112);
                    if (s.genre != outlawCountry || d > 0.7f)
                        for (int st = 1; st < 16; st += 2)
                            if (rngS.chance (0.12 * v + 0.08 * d)) S.note ((float) (o + st), 0.5f, drum::snare, 38);   // ghost notes
                    break;
                case folk:
                    if (d > 0.3f) { S.note ((float) (o + 4), 1.0f, drum::rim, 80); S.note ((float) (o + 12), 1.0f, drum::rim, 80); }
                    break;
                default:   // cinematic: build-up roll in the last bar
                    if (lastBar && d > 0.45f)
                        for (int st = 8; st < 16; ++st)
                            for (int sub = 0; sub < 2; ++sub) S.note ((float) (o + st) + 0.5f * (float) sub, 0.5f, drum::snare, 40 + (st - 8) * 9);
                    break;
            }
            if (lastBar && s.genre != cinematic && s.genre != folk && rngS.chance (0.35 * v + 0.15 * d))
                for (int st = 12; st < 16; ++st)
                    S.note ((float) (o + st), 0.5f, drum::snare, 70 + (st - 12) * 12);   // fill
        }

        // ---- hats ----
        if (write[hats])
        {
            const bool straight16 = d > 0.38f;
            switch (s.genre)
            {
                case trap: case popTrap: case drill:
                {
                    const double rollChance = (s.genre == popTrap ? 0.2 : 0.4) * v;
                    for (int beat = 0; beat < 4; ++beat)
                    {
                        const int bs = o + beat * 4;
                        if (rngH.chance (rollChance))
                        {
                            const bool triplet = rngH.chance (0.5);
                            const int divs = triplet ? 6 : 8;   // per beat: 16th triplets or 32nds
                            for (int k = 0; k < divs; ++k)
                                H.note ((float) bs + 4.0f * (float) k / (float) divs, 0.25f, drum::hatClosed, 60 + 6 * (k % 3));
                        }
                        else if (s.genre == drill)
                        {
                            static const float drillHat[] = { 0.0f, 1.333f, 2.667f };
                            for (int k = 0; k < 3; ++k) H.note ((float) bs + drillHat[k], 0.5f, drum::hatClosed, k == 0 ? 100 : 75);
                        }
                        else
                            for (int k = 0; k < 4; k += straight16 ? 1 : 2)
                                H.note ((float) (bs + k), 0.5f, drum::hatClosed, k == 0 ? 105 : (k == 2 ? 85 : 68));
                    }
                    if (rngH.chance (0.35 * v)) H.note ((float) (o + (rngH.chance (0.5) ? 14 : 6)), 2.0f, drum::hatOpen, 85);
                    break;
                }
                case boomBap: case gospel:
                    for (int st = 0; st < 16; st += (s.genre == gospel && straight16) ? 1 : 2)
                        H.note ((float) (o + st), 0.5f, drum::hatClosed, st % 4 == 0 ? 100 : (st % 2 == 0 ? 80 : 60));
                    if (rngH.chance (0.4 * v)) H.note ((float) (o + 14), 2.0f, drum::hatOpen, 80);
                    break;
                case outlawCountry:
                    for (int st = 0; st < 16; st += 2) H.note ((float) (o + st), 0.5f, st % 4 == 2 ? drum::hatClosed : drum::hatPedal, st % 4 == 2 ? 90 : 70);
                    break;
                case folk:
                    break;   // shaker on the PERC track instead
                default:
                    if (d > 0.75f) for (int st = 0; st < 16; st += 2) H.note ((float) (o + st), 0.5f, drum::hatClosed, 55);
                    break;
            }
        }

        // ---- perc ----
        if (write[perc])
        {
            if (phraseStart && (s.genre == cinematic || s.genre == gospel || (bars >= 4 && rngP.chance (0.5))))
                P.note ((float) o, 8.0f, drum::crash, 100);
            switch (s.genre)
            {
                case outlawCountry:
                    if (d > 0.5f) { P.note ((float) (o + 4), 1.0f, drum::tamb, 85); P.note ((float) (o + 12), 1.0f, drum::tamb, 85); }
                    break;
                case folk:
                    if (d > 0.3f) for (int st = 0; st < 16; st += 2) P.note ((float) (o + st), 1.0f, drum::shaker, st % 4 == 0 ? 90 : 65);
                    if (d > 0.7f) { P.note ((float) (o + 4), 1.0f, drum::tamb, 70); P.note ((float) (o + 12), 1.0f, drum::tamb, 70); }
                    break;
                case popTrap:
                    if (d > 0.6f) for (int st = 0; st < 16; ++st) P.note ((float) (o + st), 0.5f, drum::shaker, st % 2 == 0 ? 60 : 40);
                    break;
                case trap: case drill:
                    if (rngP.chance (0.3 * v)) P.note ((float) (o + 3 + 8 * rngP.below (2)), 1.0f, drum::cowbell, 70);
                    break;
                case cinematic:
                    if (d > 0.25f)
                    {
                        static const int toms[] = { drum::tomLow, drum::tomMid, drum::tomLow, drum::tomHigh };
                        for (int st = 0; st < 16; st += 4)
                            if (st == 0 || rngP.chance (0.3 + 0.5 * d)) P.note ((float) (o + st + (st > 0 && rngP.chance (0.3 * v) ? 2 : 0)), 2.0f, toms[(st / 4) % 4], st == 0 ? 120 : 90);
                    }
                    break;
                default: break;
            }
            if (lastBar && (s.genre == gospel || s.genre == boomBap) && rngP.chance (0.4 * v))
            {
                static const int toms[] = { drum::tomHigh, drum::tomMid, drum::tomLow, drum::tomLow };
                for (int k = 0; k < 4; ++k) P.note ((float) (o + 12 + k), 1.0f, toms[k], 95);
            }
        }
    }
}

//==============================================================================
// Bass
//==============================================================================
void bassFor (const ComposeSettings& s, Pattern& pat, const std::vector<Chord>& chords, Rng& rng)
{
    const int steps = s.bars * 16;
    Writer W { pat.tracks[bass], s, rng, bass, steps };
    const float v = s.variation, d = s.density;
    auto rootPitch = [] (const Chord& c, int lo) { int p = c.root; while (p < lo) p += 12; return p; };
    const bool sub808 = s.genre == trap || s.genre == drill || s.genre == popTrap || s.genre == cinematic;
    const int lo = sub808 ? 30 : 36;   // F#1 .. / C2 ..

    if (s.genre == trap || s.genre == drill || s.genre == popTrap || s.genre == boomBap)
    {
        // follow the kick (it is already written for this pattern) - the 808 / bass and kick lock together
        std::vector<float> hits;
        const auto& kt = pat.tracks[kick];
        for (int i = 0; i < kt.count; ++i) hits.push_back (std::round (kt.notes[(size_t) i].start * 4.0f) / 4.0f);
        if (hits.empty()) for (int b = 0; b < s.bars; ++b) { hits.push_back ((float) (b * 16)); hits.push_back ((float) (b * 16 + 10)); }
        if (s.genre == popTrap && d > 0.5f) for (int b = 0; b < s.bars; ++b) hits.push_back ((float) (b * 16 + 8));
        std::sort (hits.begin(), hits.end());
        hits.erase (std::unique (hits.begin(), hits.end()), hits.end());
        for (size_t i = 0; i < hits.size(); ++i)
        {
            const float st = hits[i];
            const float next = i + 1 < hits.size() ? hits[i + 1] : (float) steps;
            const auto& c = chordAt (chords, st);
            int p = rootPitch (c, lo);
            const bool lastInBar = std::fmod (next, 16.0f) < std::fmod (st, 16.0f) || next - st > 6.0f;
            if (lastInBar && rng.chance (0.35 * v)) p += rng.chance (0.5) ? 12 : 7;
            float len = std::max (0.5f, next - st - 0.15f);
            if (s.genre == boomBap) len = std::min (len, 3.0f);
            // 808 slide: overlap the next note so the voice glides instead of retriggering
            if (s.genre != boomBap && i + 1 < hits.size() && rng.chance ((s.genre == drill ? 0.55 : 0.25) * (0.4 + v)))
                len = next - st + 0.35f;
            W.note (st, len, p, s.genre == boomBap ? 100 : 112);
        }
        return;
    }

    for (const auto& c : chords)
    {
        const int r = rootPitch (c, lo);
        const int fifth = r + c.tones[2];
        const int fifthAlt = fifth - 12 >= lo - 5 ? fifth - 12 : fifth;   // alternating bass: the fifth below when it fits
        for (float bs = c.start; bs < c.start + c.length; bs += 16.0f)
        {
            const float barEnd = std::min (c.start + c.length, bs + 16.0f);
            const bool chordEndsHere = barEnd >= c.start + c.length - 0.01f;
            const auto& nextC = chordAt (chords, std::fmod (barEnd, (float) steps));
            const int nextRoot = rootPitch (nextC, lo);
            switch (s.genre)
            {
                case outlawCountry:
                {
                    // boom-chick alternating bass, walk-up into the next chord
                    W.note (bs, 3.5f, r, 105);
                    if (barEnd - bs > 8.0f) W.note (bs + 8, 3.5f, fifthAlt, 95);
                    if (chordEndsHere && barEnd - bs >= 16.0f && nextRoot != r && rng.chance (0.4 + 0.5 * v))
                    {
                        const int dir = nextRoot > r ? 1 : -1;
                        W.note (bs + 12, 1.8f, nextRoot - 2 * dir, 90);
                        W.note (bs + 14, 1.8f, nextRoot - dir, 92);
                    }
                    else if (barEnd - bs > 12.0f && d > 0.55f)
                        W.note (bs + 12, 3.5f, r + 12, 85);
                    break;
                }
                case folk:
                    W.note (bs, 7.0f, r, 95);
                    if (d > 0.4f && barEnd - bs > 8.0f) W.note (bs + 8, 7.0f, fifthAlt, 85);
                    break;
                case gospel:
                {
                    W.note (bs, 3.0f, r, 108);
                    if (barEnd - bs > 6.0f) W.note (bs + 6, 1.5f, r + 12, 90);
                    if (barEnd - bs > 8.0f) W.note (bs + 8, 2.0f, fifth, 95);
                    if (barEnd - bs > 10.0f && d > 0.5f) W.note (bs + 10, 2.0f, r + c.tones[1], 88);
                    if (chordEndsHere && barEnd - bs >= 16.0f)
                        W.note (bs + 14, 1.8f, nextRoot + (rng.chance (0.5) ? -1 : 1), 96);   // chromatic approach
                    break;
                }
                default:   // cinematic: long low roots
                    W.note (bs, std::min (16.0f, barEnd - bs) - 0.1f, r, 100);
                    if (d > 0.6f && rng.chance (0.5)) W.note (bs + 8, std::min (8.0f, barEnd - bs - 8.0f) - 0.1f, r + 12, 70);
                    break;
            }
        }
    }
}

//==============================================================================
// Keys / pad
//==============================================================================
void keysFor (const ComposeSettings& s, Pattern& pat, const std::vector<Chord>& chords, Rng& rng)
{
    const int steps = s.bars * 16;
    Writer W { pat.tracks[keys], s, rng, keys, steps };
    const float d = s.density, v = s.variation;
    if ((s.genre == trap && d < 0.5f) || s.genre == drill) return;

    std::vector<int> prev;
    for (const auto& c : chords)
    {
        auto vc = voiceChord (c, prev, 53, 77, 64, s.genre == folk ? 3 : 5);
        prev = vc;
        for (float bs = c.start; bs < c.start + c.length; bs += 16.0f)
        {
            const float span = std::min (16.0f, c.start + c.length - bs);
            auto chord = [&] (float at, float len, int vel, float strum)
            {
                if (at - bs >= span) return;
                len = std::min (len, span - (at - bs));
                for (size_t k = 0; k < vc.size(); ++k) W.note (at + strum * (float) k, len - strum * (float) k, vc[k], vel - (int) k * 3);
            };
            switch (s.genre)
            {
                case trap:          chord (bs, 8.0f, 70, 0.0f); chord (bs + 8, 8.0f, 64, 0.0f); break;
                case popTrap:       for (int st : { 0, 3, 6, 10, 12 }) chord (bs + (float) st, st == 12 ? 4.0f : 2.0f, 82, 0.0f); break;
                case boomBap:       chord (bs, 3.0f, 85, 0.0f); chord (bs + 6, 2.0f, 75, 0.0f); if (d > 0.5f) chord (bs + 10, 4.0f, 78, 0.0f); break;
                case outlawCountry:
                    if (d > 0.6f) chord (bs, 3.0f, 85, 0.05f);
                    chord (bs + 4, 3.0f, 88, 0.05f); chord (bs + 12, 3.0f, 88, 0.05f);
                    if (rng.chance (0.4 * v)) chord (bs + 10, 1.5f, 70, 0.04f);
                    break;
                case folk:
                {
                    // finger-picked arpeggio: root, 5th, 3rd, 5th, octave ...
                    static const int order[] = { 0, 2, 1, 2, 3, 2, 1, 2 };
                    const int root = vc.front();
                    std::vector<int> tones { chordToneNear (c, 0, root), chordToneNear (c, 1, root + 4), chordToneNear (c, 2, root + 7), chordToneNear (c, 0, root + 12) };
                    for (int k = 0; k < 8; ++k)
                        if ((float) (2 * k) < span) W.note (bs + (float) (2 * k), 3.0f, tones[(size_t) order[k]], k == 0 ? 92 : 74);
                    break;
                }
                case gospel:
                    chord (bs, 4.0f, 90, 0.0f); chord (bs + 6, 2.0f, 78, 0.0f); chord (bs + 10, 2.0f, 80, 0.0f);
                    if (d > 0.5f) chord (bs + 14, 2.0f, 72, 0.0f);
                    break;
                default:   // cinematic: slow rising arpeggio over two octaves
                {
                    std::vector<int> arp;
                    for (int o = 0; o < 2; ++o) for (int p : vc) arp.push_back (p + 12 * o);
                    const int stepSize = d > 0.55f ? 2 : 4;
                    for (int k = 0; k * stepSize < (int) span; ++k)
                        W.note (bs + (float) (k * stepSize), (float) stepSize * 2.0f, arp[(size_t) (k % (int) arp.size())], 70 + (k % 4 == 0 ? 15 : 0));
                    break;
                }
            }
        }
    }
}

void padFor (const ComposeSettings& s, Pattern& pat, const std::vector<Chord>& chords, Rng& rng)
{
    const int steps = s.bars * 16;
    Writer W { pat.tracks[pad], s, rng, pad, steps };
    const float d = s.density;
    const bool want = s.genre == trap || s.genre == drill || s.genre == cinematic || s.genre == popTrap
                   || (s.genre == gospel && d > 0.4f) || ((s.genre == folk || s.genre == outlawCountry) && d > 0.65f)
                   || (s.genre == boomBap && d > 0.7f);
    if (! want) return;
    std::vector<int> prev;
    for (const auto& c : chords)
    {
        auto vc = voiceChord (c, prev, 50, 74, 61, 4);
        prev = vc;
        for (int p : vc) W.note (c.start, c.length - 0.1f, p, 80);
    }
}

//==============================================================================
// Lead melody: a one-bar motif (rhythm + scale-step contour) restated over each chord
//==============================================================================
void leadFor (const ComposeSettings& s, Pattern& pat, const std::vector<Chord>& chords, Rng& rng)
{
    const int steps = s.bars * 16;
    Writer W { pat.tracks[lead], s, rng, lead, steps };
    const float d = s.density, v = s.variation;
    const float minDensity = s.genre == gospel ? 0.7f : (s.genre == outlawCountry ? 0.55f : (s.genre == boomBap ? 0.5f : 0.3f));
    if (d < minDensity) return;

    const auto& sc = Composer::scaleSteps (s.scale);
    const int n = (int) sc.size();
    auto degreePitch = [&] (int deg) { const int o = (int) std::floor ((double) deg / n); return s.key + 12 * o + sc[(size_t) (((deg % n) + n) % n)]; };
    auto nearestDegree = [&] (int pitch)
    {
        int best = 0, bestDist = 1000;
        for (int deg = -2 * n; deg < 10 * n; ++deg)
            if (const int dd = std::abs (degreePitch (deg) - pitch); dd < bestDist) { bestDist = dd; best = deg; }
        return best;
    };

    // rhythm motif (positions in a bar) by genre
    std::vector<float> rhythm;
    const bool sparse = s.genre == cinematic || s.genre == outlawCountry || s.genre == gospel;
    for (int st = 0; st < 16; ++st)
    {
        double p;
        if (st % 8 == 0) p = 0.75;
        else if (st % 4 == 0) p = 0.5;
        else if (st % 2 == 0) p = sparse ? 0.12 : 0.38;
        else p = sparse ? 0.0 : 0.18;
        p *= 0.45 + 0.9 * d;
        if (s.genre == outlawCountry && st < 8) p *= 0.3;   // answer phrases in the second half of the bar
        if (rng.chance (p)) rhythm.push_back ((float) st);
    }
    if (rhythm.empty()) rhythm = { 0.0f, 8.0f };
    std::vector<int> contour;   // scale-step moves between notes
    for (size_t i = 0; i < rhythm.size(); ++i)
    {
        const double r = rng.unit();
        contour.push_back (r < 0.35 ? 1 : (r < 0.7 ? -1 : (r < 0.82 ? 2 : (r < 0.94 ? -2 : 0))));
    }

    const int centre = s.genre == cinematic ? 69 : 74;
    int prevPitch = centre;
    const int phraseLen = s.bars >= 4 ? 4 : s.bars;
    for (int b = 0; b < s.bars; ++b)
    {
        const int inPhrase = b % phraseLen;
        const bool answer = inPhrase == phraseLen - 1 && phraseLen > 1;
        const auto& c = chordAt (chords, (float) (b * 16));
        // anchor: the chord tone nearest the previous phrase position
        int anchor = chordToneNear (c, rng.chance (0.5) ? 1 : 2, prevPitch);
        int deg = nearestDegree (anchor);
        for (size_t i = 0; i < rhythm.size(); ++i)
        {
            float st = rhythm[i];
            if (answer && i + 1 == rhythm.size()) st = std::min (st, 12.0f);
            const float next = i + 1 < rhythm.size() ? rhythm[i + 1] : 16.0f;
            int move = contour[i];
            if (b % 2 == 1 && i + 2 >= rhythm.size() && rng.chance (0.5 + 0.4 * v)) move = rng.chance (0.5) ? -move : move + 1;   // variation
            deg += i == 0 ? 0 : move;
            int p = degreePitch (deg);
            const bool strong = std::fmod (st, 4.0f) < 0.01f;
            if (strong)   // land strong beats on chord tones
            {
                const auto& cc = chordAt (chords, (float) (b * 16) + st);
                p = chordToneNear (cc, rng.below ((int) cc.tones.size()), p);
                deg = nearestDegree (p);
            }
            while (p > centre + 12) { deg -= n; p = degreePitch (deg); }
            while (p < centre - 10) { deg += n; p = degreePitch (deg); }
            float len = next - st;
            if (s.genre == trap || s.genre == drill || s.genre == popTrap) len = std::min (len, rng.chance (0.6) ? 1.5f : 3.0f);
            if (answer && i + 1 == rhythm.size()) { p = chordToneNear (c, 0, p); len = 16.0f - st; }
            W.note ((float) (b * 16) + st, std::max (0.5f, len - 0.1f), p, strong ? 100 : 84);
            prevPitch = p;
        }
    }
}

std::array<int, kTracks> genreSounds (int genre)
{
    // bass, keys, lead, pad sound types (indices into typeChoices); drums have none
    switch (genre)
    {
        case trap:          return { 0, 0, 0, 0, 0, 1, 3, 0 };
        case popTrap:       return { 0, 0, 0, 0, 0, 3, 0, 0 };
        case drill:         return { 0, 0, 0, 0, 0, 1, 2, 2 };
        case boomBap:       return { 0, 0, 0, 0, 3, 0, 1, 0 };
        case outlawCountry: return { 0, 0, 0, 0, 3, 3, 1, 1 };
        case folk:          return { 0, 0, 0, 0, 3, 3, 3, 1 };
        case gospel:        return { 0, 0, 0, 0, 2, 2, 0, 2 };
        default:            return { 0, 0, 0, 0, 0, 1, 3, 1 };
    }
}
} // namespace

//==============================================================================
const std::vector<int>& Composer::scaleSteps (int scaleIndex)
{
    return scaleTable()[(size_t) std::clamp (scaleIndex, 0, (int) scaleTable().size() - 1)];
}

double Composer::genreTempo (int genre, uint32_t seed)
{
    static constexpr double lo[kNumGenres] = { 132, 120, 138, 86, 92, 84, 72, 70 };
    static constexpr double hi[kNumGenres] = { 150, 140, 146, 96, 118, 108, 92, 96 };
    Rng r (seed * 7919u + 13u);
    const int g = std::clamp (genre, 0, kNumGenres - 1);
    return std::round (r.range (lo[g], hi[g]));
}

double Composer::genreSwing (int genre)
{
    switch (genre)
    {
        case boomBap: return 58.0;
        case gospel:  return 56.0;
        case folk:    return 52.0;
        default:      return 50.0;
    }
}

void Composer::compose (const ComposeSettings& s, const Pattern& current, ComposeResult& out)
{
    out.pattern.clear();
    out.pattern.bars = std::clamp (s.bars, 1, kMaxBars);
    Rng harmony (s.harmonySeed);
    const auto chords = buildChords (s, harmony);
    out.bpm = genreTempo (s.genre, s.harmonySeed);
    out.soundType = genreSounds (s.genre);

    out.chordNames.clear();
    for (auto& c : chords) out.chordNames += (out.chordNames.empty() ? "" : " | ") + c.name;

    auto rngFor = [&s] (int t) { return Rng (((uint64_t) s.trackSeed[(size_t) t] << 20) ^ (uint64_t) (t * 104729 + 7)); };
    std::array<bool, kTracks> write {};
    for (int t = 0; t < kTracks; ++t) write[(size_t) t] = ! s.locked[(size_t) t];

    // locked tracks keep their notes (bass and keys still follow the new kick / chords only when unlocked)
    for (int t = 0; t < kTracks; ++t)
        if (s.locked[(size_t) t])
        {
            out.pattern.tracks[(size_t) t] = current.tracks[(size_t) t];
            auto& tn = out.pattern.tracks[(size_t) t];
            int keep = 0;
            for (int i = 0; i < tn.count; ++i)
                if (tn.notes[(size_t) i].start < (float) out.pattern.lengthSteps()) tn.notes[(size_t) keep++] = tn.notes[(size_t) i];
            tn.count = keep;
        }

    Rng rk = rngFor (kick), rs = rngFor (snare), rh = rngFor (hats), rp = rngFor (perc);
    drumsFor (s, out.pattern, rk, rs, rh, rp, write);
    if (write[bass]) { Rng r = rngFor (bass); bassFor (s, out.pattern, chords, r); }
    if (write[keys]) { Rng r = rngFor (keys); keysFor (s, out.pattern, chords, r); }
    if (write[pad])  { Rng r = rngFor (pad);  padFor (s, out.pattern, chords, r); }
    if (write[lead]) { Rng r = rngFor (lead); leadFor (s, out.pattern, chords, r); }

    for (auto& t : out.pattern.tracks) t.sort();
}

//==============================================================================
// Song form
//==============================================================================
namespace
{
struct SectionPlan { Section kind; int bars; };

std::vector<SectionPlan> formFor (int genre)
{
    switch (genre)
    {
        case trap: case popTrap: case drill:
            return { { kIntro, 8 }, { kHook, 16 }, { kVerse, 16 }, { kHook, 16 }, { kVerse, 16 }, { kHook, 16 }, { kOutro, 8 } };
        case boomBap:
            return { { kIntro, 4 }, { kVerse, 16 }, { kHook, 8 }, { kVerse, 16 }, { kHook, 8 }, { kVerse, 16 }, { kHook, 8 }, { kOutro, 4 } };
        case outlawCountry: case folk:
            return { { kIntro, 4 }, { kVerse, 16 }, { kHook, 8 }, { kVerse, 16 }, { kHook, 8 }, { kBridge, 8 }, { kHook, 8 }, { kOutro, 4 } };
        case gospel:
            return { { kIntro, 4 }, { kVerse, 16 }, { kHook, 8 }, { kVerse, 16 }, { kHook, 8 }, { kBridge, 8 }, { kHook, 16 }, { kOutro, 8 } };
        default:   // cinematic: slow build
            return { { kIntro, 8 }, { kVerse, 16 }, { kBridge, 8 }, { kHook, 16 }, { kOutro, 8 } };
    }
}

constexpr std::uint8_t bit (int t) { return static_cast<std::uint8_t> (1u << t); }
constexpr std::uint8_t kDrums = bit (kick) | bit (snare) | bit (hats) | bit (perc);
constexpr std::uint8_t kAll = 0xFF;

/** Tracks that play in a section, before the per-song variations. */
std::uint8_t maskFor (int genre, Section kind)
{
    const bool urban = genre == trap || genre == popTrap || genre == drill || genre == boomBap;
    switch (kind)
    {
        case kIntro:
            if (genre == cinematic) return bit (pad) | bit (keys);
            if (urban) return bit (keys) | bit (pad) | bit (lead);
            return bit (keys) | bit (perc);                                   // country / folk / gospel: the band counts in
        case kVerse:
            return static_cast<std::uint8_t> (kAll & ~bit (lead));            // leave room for the voice
        case kHook:
            return kAll;
        case kBridge:
            return urban ? static_cast<std::uint8_t> (bit (keys) | bit (pad) | bit (lead) | bit (hats))
                         : static_cast<std::uint8_t> (bit (keys) | bit (pad) | bit (bass) | bit (perc));
        case kDrop:
            return static_cast<std::uint8_t> (kAll & ~(bit (kick) | bit (bass)));
        case kOutro:
        default:
            return genre == cinematic ? static_cast<std::uint8_t> (bit (pad) | bit (keys) | bit (perc))
                                      : static_cast<std::uint8_t> (bit (keys) | bit (pad));
    }
}
} // namespace

Arrangement Composer::arrange (int genre, const Pattern& pattern, uint32_t seed, float variation)
{
    Arrangement a;
    const int P = std::clamp (pattern.bars, 1, kMaxBars);
    std::uint8_t present = 0;
    for (int t = 0; t < kTracks; ++t)
        if (pattern.tracks[(size_t) t].count > 0) present |= bit (t);
    if (present == 0) return a;   // nothing to arrange: loop

    Rng rng (((uint64_t) seed << 16) ^ 0xA77A11u);
    const bool urban = genre == trap || genre == popTrap || genre == drill || genre == boomBap;
    int verseNo = 0;
    for (const auto& sp : formFor (genre))
    {
        const int len = std::max (P, ((sp.bars + P - 1) / P) * P);   // whole patterns
        std::uint8_t m = maskFor (genre, sp.kind);
        if (sp.kind == kVerse)
        {
            ++verseNo;
            if (verseNo == 1 && rng.chance (0.5 * variation + 0.2)) m &= static_cast<std::uint8_t> (~bit (perc));   // first verse a bit thinner
            if (verseNo >= 2 && rng.chance (0.4 * variation)) m &= static_cast<std::uint8_t> (~bit (pad));
        }
        if (sp.kind == kIntro && urban && rng.chance (0.5)) m |= bit (hats);
        if ((m & present) == 0) m = kAll;   // the section would be silent: play everything
        for (int b = 0; b < len && a.bars < kMaxSongBars; ++b)
        {
            a.mask[(size_t) a.bars] = m;
            a.section[(size_t) a.bars] = sp.kind;
            ++a.bars;
        }
    }
    // Drop: the bar before each hook loses kick and bass (urban genres; more likely with variation).
    if (urban)
        for (int b = 1; b < a.bars; ++b)
            if (a.section[(size_t) b] == kHook && a.section[(size_t) (b - 1)] != kHook && rng.chance (0.45 + 0.5 * variation))
            {
                a.mask[(size_t) (b - 1)] = static_cast<std::uint8_t> (a.mask[(size_t) (b - 1)] & maskFor (genre, kDrop));
                a.section[(size_t) (b - 1)] = kDrop;
            }
    // Last bar of the outro: let it ring out (melodic tracks only).
    if (a.bars > 0)
        a.mask[(size_t) (a.bars - 1)] &= static_cast<std::uint8_t> (bit (keys) | bit (pad) | bit (lead) | bit (bass));
    return a;
}

std::string Composer::describe (const Arrangement& a)
{
    if (! a.active()) return "LOOP";
    std::string out;
    int b = 0;
    while (b < a.bars)
    {
        const auto kind = a.section[(size_t) b];
        int n = 0;
        while (b < a.bars && a.section[(size_t) b] == kind) { ++b; ++n; }
        if (! out.empty()) out += " \xC2\xB7 ";
        out += std::string (kSectionNames[kind]) + " " + std::to_string (n);
    }
    return out;
}
} // namespace hc::forge
