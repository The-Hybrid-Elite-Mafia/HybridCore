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
} // namespace hc::fx
