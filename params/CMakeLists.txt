# =============================================================================
#  hc::params — thread-safe parameter tree, settings store, mixer parameter set
# =============================================================================
add_library(hc_params STATIC
    src/ParameterTree.cpp
    src/Settings.cpp
    src/MixerParams.cpp)
add_library(hc::params ALIAS hc_params)

target_include_directories(hc_params PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hc_params PUBLIC hc::build_flags hc::core hc::dsp)
