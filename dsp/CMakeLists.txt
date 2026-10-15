# =============================================================================
#  hc::dsp — AVX2 kernels and the 13-stem routing matrix (no engine/io deps)
# =============================================================================
add_library(hc_dsp STATIC
    src/Kernels.cpp
    src/StemMatrix.cpp)
add_library(hc::dsp ALIAS hc_dsp)

target_include_directories(hc_dsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hc_dsp PUBLIC hc::build_flags hc::core)
