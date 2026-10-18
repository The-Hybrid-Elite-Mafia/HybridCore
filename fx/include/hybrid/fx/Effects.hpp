#pragma once
// Factory functions of every HEM effect (one .cpp each in fx/src).
#include "hybrid/fx/Fx.hpp"

namespace hc::fx {
[[nodiscard]] std::unique_ptr<Effect> makeVoxOne();
[[nodiscard]] std::unique_ptr<Effect> makeEq6();
[[nodiscard]] std::unique_ptr<Effect> makeBusPress();
[[nodiscard]] std::unique_ptr<Effect> makeSubStack();
[[nodiscard]] std::unique_ptr<Effect> makeStrip();
[[nodiscard]] std::unique_ptr<Effect> makeCalm();
[[nodiscard]] std::unique_ptr<Effect> makeClip();
[[nodiscard]] std::unique_ptr<Effect> makeCeiling();
[[nodiscard]] std::unique_ptr<Effect> makeHeat();
[[nodiscard]] std::unique_ptr<Effect> makeBloom();
[[nodiscard]] std::unique_ptr<Effect> makePunch();
[[nodiscard]] std::unique_ptr<Effect> makeTriBand();
[[nodiscard]] std::unique_ptr<Effect> makeWide();
[[nodiscard]] std::unique_ptr<Effect> makeHush();
[[nodiscard]] std::unique_ptr<Effect> makeDeEss();
[[nodiscard]] std::unique_ptr<Effect> makeDouble();
[[nodiscard]] std::unique_ptr<Effect> makeDesk();
[[nodiscard]] std::unique_ptr<Effect> makeDust();
[[nodiscard]] std::unique_ptr<Effect> makeTune();
[[nodiscard]] std::unique_ptr<Effect> makeBalance();
[[nodiscard]] std::unique_ptr<Effect> makeEcho();
[[nodiscard]] std::unique_ptr<Effect> makeSwirlKiller();

// HEM TUNE's note finder (exposed for tests): nearest allowed MIDI note for key (0 = C) and
// scale (0 chromatic, 1 major, 2 minor).
[[nodiscard]] float tuneSnap(float midi, int key, int scale) noexcept;
} // namespace hc::fx
