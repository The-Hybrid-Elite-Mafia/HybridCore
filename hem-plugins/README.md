# The HEM plugin series

Each plugin has its **own folder**: its engine, its one config file of guesses, its tests.
All are C++23, no JUCE, built on The Hybrid Core. Names and sound are our own.

| Folder | Plugin | State |
|---|---|---|
| `hem-compressor/` | HEM COMPRESSOR | Engine built, 54 measured checks, in the FX rack and the VST3 suite. Its own HEM faceplate (ui/src/FxPanelCompressor*.cpp): brushed titanium, crown badge and ghost, needle GR meter, IN / OUT LED ladders, used in the FX rack, the VST3 editor and the CI snapshots. Not yet seen on Steven's own screen. |

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
