#include "hybrid/fx/Effects.hpp"

namespace hc::fx {

std::span<const EffectInfo> effects() noexcept {
    // UIDs are fixed forever once released: hosts save sessions by them.
    static const EffectInfo kList[] = {
        {"HEM VOX ONE", "Fx|Dynamics", "One knob vocal: cleans the lows, evens the level, tames S, adds presence", {0x15EAE6EB, 0x6C1E46AE, 0xB5378FAB, 0x6C39815B}, {0x659A5EE5, 0xBEE04CBE, 0xA7F171E9, 0x5A9A1BD1}, &makeVoxOne},
        {"HEM EQ-6", "Fx|EQ", "Six-band EQ: low cut, low shelf, two bells, high shelf, high cut", {0x8B6848EB, 0x49704DEC, 0xAC973C04, 0xDE669E3C}, {0x39358E2B, 0x5DE84932, 0xA5345BD8, 0x61496BBD}, &makeEq6},
        {"HEM BUS PRESS", "Fx|Dynamics", "Bus compressor that glues a mix or drum bus, with mix knob and side-chain low cut", {0xDECAEF0E, 0xA54A48ED, 0xBC492CE1, 0x72EFF89A}, {0xED8B0718, 0x64B14723, 0x931B22C2, 0x1C3BBA19}, &makeBusPress},
        {"HEM SUBSTACK", "Fx|Distortion", "Bass enhancer: deeper sub and harmonics so 808s and bass read on small speakers", {0x3E8D9CFA, 0x0542449B, 0x97E4E757, 0x44209D65}, {0x67CB3982, 0xA5BC48C9, 0xB825328E, 0xC015BF18}, &makeSubStack},
        {"HEM STRIP", "Fx|Dynamics", "Channel strip: low cut, gate, compressor, 3-band EQ, output", {0x07452A1C, 0xA1064064, 0x91507022, 0xFA8170F0}, {0x110A7B21, 0x18D04F05, 0xB6F822F9, 0x287537AF}, &makeStrip},
        {"HEM CALM", "Fx|Restoration", "Resonance tamer: finds harsh ringing frequencies as they happen and turns only those down", {0x0041BAC1, 0x947545ED, 0xAC5AA20F, 0x2F30782F}, {0x50D86B1F, 0x27CF47B5, 0xBC408C54, 0x4248DB65}, &makeCalm},
        {"HEM CLIP", "Fx|Distortion", "Clipper (4x oversampled): shaves peaks off so the mix gets louder without pumping", {0x71AE786D, 0x4FEF4FC4, 0x846A80DF, 0x80AAD1C7}, {0x4EF974D2, 0xE861495D, 0xA4164183, 0x37B81253}, &makeClip},
        {"HEM CEILING", "Fx|Mastering", "Mastering limiter with true-peak ceiling (4x oversampled), look-ahead", {0x4C20BE50, 0x3F3545BD, 0xBAE7AC7A, 0xF24255FE}, {0x989D2777, 0x8DCA48A6, 0xBE2AF38E, 0x2653D646}, &makeCeiling},
        {"HEM HEAT", "Fx|Distortion", "Saturation: tape, tube or transistor warmth, with tone and mix", {0xB94F12A3, 0x1C984DF3, 0xB38F525D, 0x8BEEBB57}, {0xBE8EF53D, 0xD90E4084, 0xB0D8CDDD, 0x6B0C9683}, &makeHeat},
        {"HEM BLOOM", "Fx|Reverb", "Reverb that steps out of the way: ducks only where it clashes with the dry sound", {0xDEFCFD04, 0xC08347E1, 0x8C4BD275, 0x4806E0A0}, {0x263D3FFD, 0xAAFF4D86, 0xA400D676, 0x75BA246C}, &makeBloom},
    };
    return kList;
}

} // namespace hc::fx
