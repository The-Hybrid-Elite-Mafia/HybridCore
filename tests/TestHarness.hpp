#pragma once
// Minimal test harness shared by all hc_tests translation units.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace hc::test {

inline int gFails  = 0;
inline int gChecks = 0;

// Heap allocation counter (operator new replacement lives in AllocTracker.cpp).
// Counting is enabled only inside an AllocScope.
inline std::atomic<bool>          gTrackAllocs{false};
inline std::atomic<std::uint64_t> gAllocCount{0};

struct AllocScope {
    AllocScope() noexcept { gAllocCount.store(0); gTrackAllocs.store(true); }
    ~AllocScope() { gTrackAllocs.store(false); }
    [[nodiscard]] std::uint64_t count() const noexcept { return gAllocCount.load(); }
};

void testRing();
void testConvert();
void testEngine();
void testKernels();
void testMatrix();
void testMixer();
void testZeroAllocation();
void testParams();
void testSettings();
void testMixerBinding();
void testUiImage();
void testUiLayout();
void testUiFaceplate();
void testUiFont();
void testUiParts();
void testMediaFiles();
void testChannelStrip();
void testSession();
void testForgeComposer();
void testForgeEngine();
void testForgeInMixer();
void testRemake();
void testFxCommon();
void testFxClip();
void testFxCeiling();
void testFxHeat();
void testFxDynamics();
void testFxTone();
void testFxNew();
void testFxRack();
void testFxPanel();
void testSynth();
void testExport();
void testLoudness();
void testAnalyzer();
void testLimiter();
void testStripInMatrix();
void testStripParams();
void testResampler();
void testStemLoader();
void testStemPlayback();
void testTransport();
void testConsoleDesk();
void testConsoleStepEq();
void testConsoleTwinBus();
void testConsoleColour();
void testConsoleMeter();
void testConsoleTools();
void testConsoleRig();
void testConsoleUi();
void testConsolePlayer();
void benchmarkConsole();

// Folder with faceplates/ and fonts/: <exe dir>/assets (build tree) or <exe dir>/../assets (installed).
void setExePath(const char* argv0);
[[nodiscard]] std::filesystem::path assetDir();
// Encoded test vectors (tests/data): <exe dir>/testdata.
[[nodiscard]] std::filesystem::path testDataDir();
void benchmark();

} // namespace hc::test

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++::hc::test::gChecks;                                                      \
        if (!(cond)) {                                                              \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            ++::hc::test::gFails;                                                   \
        }                                                                           \
    } while (0)
