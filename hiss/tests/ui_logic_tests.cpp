// Tests for the window logic of the Comb and Click Finder: the knob, and that the knob really changes what the engine does.
// Pure C++: runs on any machine.
#include "CombClickModel.hpp"
#include "HemControls.hpp"

#include <cstdio>

static int g_fail = 0, g_n = 0;
#define CHECK(c, msg) do { ++g_n; if (c) std::printf("  PASS  %s\n", msg); else { ++g_fail; std::printf("  FAIL  %s\n", msg); } } while (0)

int main() {
    using namespace hem;
    ui::Knob k; k.lo = 10; k.hi = 100; k.def = 40; k.step = 1; k.value = 40;
    CHECK(k.hit(k.cx, k.cy), "a click on the knob centre hits the knob");
    CHECK(!k.hit(k.cx + 500, k.cy), "a click far away does not");
    k.begin(300); k.drag(300 - 22.0f, false);
    CHECK(std::fabs(k.value - 49.0) < 1e-9, "drag up 22 px = 10 percent of the range = +9 ms");
    k.drag(300 + 5000.0f, false);
    CHECK(k.value == 10.0, "dragging far down stops at the bottom of the range");
    k.end(); k.reset();
    CHECK(k.value == 40.0, "double-click goes back to 40 ms");
    k.begin(300); k.drag(300 - 22.0f, true); k.end();
    CHECK(std::fabs(k.value - 42.0) < 1e-9 || std::fabs(k.value - 41.0) < 1e-9, "Shift = finer steps (22 px moves about 2 ms)");
    k.reset(); k.wheel(3);
    CHECK(k.value == 43.0, "wheel moves one step per notch");
    k.set(55.4);
    CHECK(k.value == 55.0, "values snap to whole milliseconds");

    // The knob decides what the engine looks for. Made-up clip: a click every 55 ms.
    const double fs = 48000.0;
    const auto x = combclick::madeUpClip(fs, 12.0, 55.0, -30.0, -40.0, 7);
    k.set(55.0);
    auto at55 = combclick::scan(x, x, fs, k.value);
    k.set(40.0);
    auto at40 = combclick::scan(x, x, fs, k.value);
    CHECK(at55.ok && at55.seam.valid && at55.seam.pattern, "knob at 55 ms: the 55 ms click train is called a pattern");
    CHECK(at40.ok && at40.seam.valid && !at40.seam.pattern, "knob at 40 ms: the same file shows no 40 ms pattern");
    CHECK(at55.periodMs == 55.0 && at40.periodMs == 40.0, "the result remembers which period the knob had");
    CHECK(!at55.seam.foldProfile.empty() && std::fabs(at55.seam.foldProfileMs - 55.0) < 0.1, "the fold picture covers the chosen period");
    float mx = 0; std::size_t arg = 0;
    for (std::size_t i = 0; i < at55.seam.foldProfile.size(); ++i) if (at55.seam.foldProfile[i] > mx) { mx = at55.seam.foldProfile[i]; arg = i; }
    const double peakMs = at55.seam.foldProfileMs * (static_cast<double>(arg) + 0.5) / static_cast<double>(at55.seam.foldProfile.size());
    const double expectMs = std::fmod(0.37 * 55.0, 55.0);
    std::printf("  info  fold picture peak at %.2f ms; click was put at %.2f ms; engine says %.2f ms; bar %.3g, peak %.3g\n", peakMs, expectMs, at55.seam.foldPhaseMs, at55.seam.foldBar, mx);
    CHECK(mx > at55.seam.foldBar, "the peak in the picture stands above the 1-in-100 bar");
    CHECK(std::fabs(peakMs - expectMs) < 1.5, "the peak sits where the click was put (within 1.5 ms)");
    const auto clean = combclick::madeUpClip(fs, 12.0, 0.0, 0.0, -40.0, 9);
    auto c = combclick::scan(clean, clean, fs, 40.0);
    CHECK(c.ok && !c.seam.pattern, "clean noise: no pattern");
    CHECK(c.seam.foldBar > 0 && !c.seam.foldProfile.empty() && *std::max_element(c.seam.foldProfile.begin(), c.seam.foldProfile.end()) < c.seam.foldBar, "clean noise: nothing in the picture crosses the bar");
    const auto rep = combclick::reportText(x, x, fs, 55.0, "made-up");
    CHECK(rep.find("55 ms fold test") != std::string::npos && rep.find("YES, a repeating pattern at this period") != std::string::npos, "the saved report uses the knob period and agrees with the screen");
    const auto tooShort = combclick::madeUpClip(fs, 0.2, 0.0, 0.0, -40.0, 1);
    CHECK(!combclick::scan(tooShort, tooShort, fs, 40.0).seam.valid, "a clip under 0.5 s is refused, not guessed");
    std::printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
