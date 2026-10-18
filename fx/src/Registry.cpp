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
        // 0.14: from the Rework Playground, rebuilt for The Hybrid Core
        {"HEM PUNCH", "Fx|Dynamics", "Transient shaper: more or less hit and ring-out, without touching the level in between", {0xAFB26AFF, 0x680844F4, 0xBECE9D53, 0xA5932CC1}, {0x3C87E5F8, 0x0C0C4D02, 0x82FE7E58, 0x9E17CA67}, &makePunch},
        {"HEM TRIBAND", "Fx|Dynamics", "Three-band compressor: control lows, mids and highs on their own", {0x52ED1297, 0x3EAD4C71, 0x9B1EA36F, 0xC93C1505}, {0xA4AD7B00, 0x9073485C, 0xB6680624, 0xED942250}, &makeTriBand},
        {"HEM WIDE", "Fx|Spatial", "Stereo width with a mono low end, side tone and a mono-safe guard", {0x81F26926, 0x7F474EF4, 0xAF183773, 0xEE8A359F}, {0xD7E14EFF, 0x4CAD4535, 0x91EB8FB5, 0xCBC95A6C}, &makeWide},
        {"HEM HUSH", "Fx|Restoration", "Hiss and noise remover: learns the noise floor by itself, leaves the music on top", {0x51ADD676, 0x242B4DEC, 0xA4328811, 0xE6089B3D}, {0x5E32AEA3, 0xAEC44579, 0x8B08A6D8, 0x49E08D5E}, &makeHush},
        {"HEM DE-ESS", "Fx|Dynamics", "Turns down harsh S and T sounds only while they jump out", {0x24D4F6EC, 0xF9804164, 0x9EFFD5ED, 0x56788523}, {0xAB08E210, 0x3FF94B8C, 0xB1A7099F, 0x57FB9ABC}, &makeDeEss},
        {"HEM DOUBLE", "Fx|Spatial", "Instant double-track: 2 or 4 drifting copies spread left and right", {0x9593BACA, 0xB5224736, 0xBEF163A9, 0x4BFE5D23}, {0x9D577807, 0x82BD4C87, 0x9C6AB105, 0x614EEC4B}, &makeDouble},
        {"HEM DESK", "Fx|Distortion", "Mixing-desk glue: channel drive (4x oversampled), weight and air, crosstalk", {0x8840553C, 0x84314413, 0x82771C4D, 0xCDCC20E9}, {0xAC335B2D, 0x63884131, 0xAA714814, 0xAD1A75DD}, &makeDesk},
        {"HEM DUST", "Fx|Distortion", "Old tape and sampler: wow, flutter, crushed bits, vinyl that follows the music", {0x55DAB920, 0xD0964612, 0xB34C9E68, 0x95B20EBF}, {0xE45A44B0, 0x205F4BB2, 0xAE4254DE, 0x7E4F4BB1}, &makeDust},
        {"HEM TUNE", "Fx|Pitch Shift", "Vocal pitch correction: pulls one voice to the nearest note of your key", {0xC6DDD6B0, 0x05CC4C2D, 0xAB96CA4B, 0xA5A9CF58}, {0xC509AEFE, 0xEF1B4AEF, 0x990224F4, 0x4494A608}, &makeTune},
        {"HEM BALANCE", "Fx|EQ", "Automatic tone balance: evens out frequency areas that stick out or are missing", {0xFCE87746, 0x6FFE4134, 0x8A749E91, 0x5790E715}, {0x45E60759, 0xA3654986, 0xA731E712, 0x1D741894}, &makeBalance},
        {"HEM ECHO", "Fx|Delay", "Stereo / ping-pong delay with tempo sync, ducking and tape wobble", {0x50BA60C8, 0x31CB4ACB, 0xB7AEA12B, 0xB330F84C}, {0x8FB25D7B, 0x2E8C4C6B, 0xA9647CCF, 0x6787AE01}, &makeEcho},
        {"HEM SWIRL KILLER", "Fx|Restoration", "Removes AI / codec swirl in the stereo sides, keeps the centre untouched", {0xF9DF44AE, 0x8D6E4EDC, 0x9B3DA8A5, 0x563A1ACD}, {0xFB001571, 0x17B74FBF, 0xBDBA25CE, 0x2DD6535D}, &makeSwirlKiller},
    };
    return kList;
}

} // namespace hc::fx
