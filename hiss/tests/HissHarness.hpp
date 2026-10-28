#pragma once
// Tiny test harness for the Hiss tests. Every check prints PASS or FAIL; every measured number is printed as "MEASURED name = value unit".
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace hc::hiss::test {

inline int gChecks = 0, gFails = 0;
inline std::atomic<bool> gTrackAllocs{false};
inline std::atomic<std::uint64_t> gAllocCount{0};

struct AllocScope {
    AllocScope() noexcept { gAllocCount.store(0); gTrackAllocs.store(true); }
    ~AllocScope() { gTrackAllocs.store(false); }
    [[nodiscard]] std::uint64_t count() const noexcept { return gAllocCount.load(); }
};

} // namespace hc::hiss::test

#define HCHECK(cond, ...)                                                                  \
    do {                                                                                   \
        ++::hc::hiss::test::gChecks;                                                       \
        if (cond) { std::printf("  PASS  "); }                                             \
        else { ++::hc::hiss::test::gFails; std::printf("  FAIL  "); }                      \
        std::printf(__VA_ARGS__);                                                          \
        std::printf("\n");                                                                 \
    } while (0)

#define HMEASURED(name, value, unit) std::printf("  MEASURED %s = %.4f %s\n", name, static_cast<double>(value), unit)
