# =============================================================================
#  HybridCore.exe — standalone 13-stem console (GUI subsystem)
# =============================================================================
configure_file(resources/HybridCore.rc.in "${CMAKE_CURRENT_BINARY_DIR}/HybridCore.rc" @ONLY)
add_executable(HybridCore WIN32
    src/main.cpp
    src/AudioHost.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/HybridCore.rc")
target_include_directories(HybridCore PRIVATE src)
target_link_libraries(HybridCore PRIVATE hc::engine hc::params hc::ui hc::io)
set_target_properties(HybridCore PROPERTIES OUTPUT_NAME "HybridCore")
