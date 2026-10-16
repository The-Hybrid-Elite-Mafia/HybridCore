# =============================================================================
#  hc::media — audio file decoding (WAV/RF64 own parser, MP3 + FLAC via dr_libs),
#  sample-rate conversion to the rig rate, background stem loader.
# =============================================================================
add_library(hc_media STATIC
    src/AudioFile.cpp
    src/Resampler.cpp
    src/StemLoader.cpp
    src/DrLibsImpl.cpp
    src/Session.cpp
    src/MixExporter.cpp)
add_library(hc::media ALIAS hc_media)

target_include_directories(hc_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Vendored dr_libs headers are third-party: SYSTEM so /external:W0 silences their warnings.
target_include_directories(hc_media SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/external/dr_libs")
# Memory-only decoding: the dr_libs file I/O code is compiled out.
target_compile_definitions(hc_media PRIVATE DR_MP3_NO_STDIO DR_FLAC_NO_STDIO)
target_link_libraries(hc_media PUBLIC hc::build_flags hc::core hc::engine hc::params)
