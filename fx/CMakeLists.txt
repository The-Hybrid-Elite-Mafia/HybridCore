# =============================================================================
#  hc::fx — the HEM effect plug-ins' DSP (stereo, in place, lock-free parameters).
#  Used by the VST3 suite (plugin/) and tested in tests/fx_*_tests.cpp.
# =============================================================================
file(GLOB HC_FX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(hc_fx STATIC ${HC_FX_SOURCES})
add_library(hc::fx ALIAS hc_fx)
target_include_directories(hc_fx PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hc_fx PUBLIC hc::build_flags hc::core)
