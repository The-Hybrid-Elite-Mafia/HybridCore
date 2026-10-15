# =============================================================================
#  HybridCore.exe — standalone 13-stem console (GUI subsystem)
#  hc_apphost   — the audio host (engine + device streams) and the stem deck, shared with hc_ui_snapshot
# =============================================================================
add_library(hc_apphost STATIC src/AudioHost.cpp src/StemDeck.cpp)
target_include_directories(hc_apphost PUBLIC src)
target_link_libraries(hc_apphost PUBLIC hc::build_flags hc::engine hc::io hc::media)

configure_file(resources/HybridCore.rc.in "${CMAKE_CURRENT_BINARY_DIR}/HybridCore.rc" @ONLY)
add_executable(HybridCore WIN32
    src/main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/HybridCore.rc")
target_include_directories(HybridCore PRIVATE src)
target_link_libraries(HybridCore PRIVATE hc_apphost hc::params hc::ui opengl32 shell32 ole32)
set_target_properties(HybridCore PROPERTIES OUTPUT_NAME "HybridCore")
hc_copy_assets(HybridCore)
