#pragma once
// HEM ECHO DECK: the plugin's public face. One factory; the engine class itself stays private in src/.
// It is an hc::fx::Effect, so The Hybrid Core's FX rack, the VST3 suite and the tests all drive it the same way.
#include "hybrid/fx/Fx.hpp"

#include <memory>

namespace hem::echodeck {
[[nodiscard]] std::unique_ptr<hc::fx::Effect> make();
} // namespace hem::echodeck
