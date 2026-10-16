# =============================================================================
#  hc::forge — BEAT FORGE: synthesized 8-track beat maker, composer, song arranger
#  (ported from Hybrid Studio 2.0.9 without JUCE)
# =============================================================================
add_library(hc_forge STATIC
    src/Forge.cpp
    src/Composer.cpp
    src/ForgeEngine.cpp)
add_library(hc::forge ALIAS hc_forge)

target_include_directories(hc_forge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hc_forge PUBLIC hc::build_flags hc::core hc::params)
