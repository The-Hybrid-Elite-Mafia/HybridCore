# =============================================================================
#  The HEM plugin series. Every plugin lives in its own folder here, with its own engine, its own
#  config file (all its ASSUMED / UNKNOWN guesses), its own tests, and its own CMakeLists.txt.
#  To add one: make hem-plugins/<name>/, copy hem-compressor's layout, add one line below.
#  Plugins are C++23, no JUCE. They only use the Effect interface from hc::fx (fx/include/hybrid/fx/Fx.hpp),
#  so The Hybrid Core's FX rack and the VST3 suite can host every one of them.
# =============================================================================
add_subdirectory(hem-compressor)
