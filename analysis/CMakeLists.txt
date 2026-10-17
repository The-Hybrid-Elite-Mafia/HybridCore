# =============================================================================
#  hc::analysis — the ANALYZER's measurements: lock-free tap of the Main output,
#  FFT waterfall, BS.1770 loudness, peaks, stereo, tone balance, plain-words checks.
#  No UI code: the 3D view lives in hc::ui.
# =============================================================================
add_library(hc_analysis STATIC
    src/Fft.cpp
    src/Analyzer.cpp)
add_library(hc::analysis ALIAS hc_analysis)

target_include_directories(hc_analysis PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hc_analysis PUBLIC hc::build_flags hc::core)
