# =============================================================================
#  hc::synth — HEM WAVE, the morphing wavetable synth (our own design and code).
#  Used by the VST3 instrument (plugin/src/HemWaveVst3.cpp); tested in tests/synth_tests.cpp.
# =============================================================================
file(GLOB HC_SYNTH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(hc_synth STATIC ${HC_SYNTH_SOURCES})
add_library(hc::synth ALIAS hc_synth)
target_include_directories(hc_synth PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hc_synth PUBLIC hc::build_flags hc::core)
