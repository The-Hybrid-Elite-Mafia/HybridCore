# The HEM plugin series

Each plugin has its **own folder**: its engine, its one config file of guesses, its tests.
All are C++23, no JUCE, built on The Hybrid Core. Names and sound are our own.

| Folder | Plugin | State |
|---|---|---|
| `hem-compressor/` | HEM COMPRESSOR | Engine built, 33 measured checks in CI, in the FX rack and the VST3 suite. Faceplate: the console's automatic one. A custom HEM faceplate is not built yet. |

## Layout of one plugin
```
hem-plugins/<name>/
  CMakeLists.txt
  include/hem/<name>/<Name>Config.hpp   every ASSUMED / UNKNOWN guess, in ONE file
  include/hem/<name>/Hem<Name>.hpp      public face: make() returns an hc::fx::Effect
  src/                                  the engine (no allocation on the audio thread)
  tests/                                measured checks, built into hc_tests
```
## To add a plugin
Copy `hem-compressor/`, rename it, then add one `add_subdirectory(<name>)` line to `hem-plugins/CMakeLists.txt`
and one entry in `fx/src/Registry.cpp` (a new, never-reused ID set).

## Not decided / not known
- How the Microsoft Store accepts and packages VST3 plugins: not checked yet.
- The Hybrid Core is GPLv3. Plugins built on it are subject to those terms when shipped.
  Whether that fits a Store release is a question for a lawyer, not something this repo answers.
