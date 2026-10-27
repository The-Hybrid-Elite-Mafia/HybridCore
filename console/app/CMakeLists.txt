# =============================================================================
#  HEMDigitalConsole.exe — the HEM DIGITAL CONSOLE app (GUI subsystem), and
#  hem_console_snapshot — renders every faceplate off-screen with the live rig (CI screenshots, works on the rig too)
# =============================================================================
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" HEM_CONSOLE_VERSION)
string(STRIP "${HEM_CONSOLE_VERSION}" HEM_CONSOLE_VERSION)
string(REPLACE "." ";" _v "${HEM_CONSOLE_VERSION}")
list(GET _v 0 HEM_CONSOLE_V1)
list(GET _v 1 HEM_CONSOLE_V2)
list(GET _v 2 HEM_CONSOLE_V3)

add_library(hem_console_host STATIC src/ConsoleHost.cpp)
target_include_directories(hem_console_host PUBLIC src)
target_link_libraries(hem_console_host PUBLIC hc::build_flags hc::console_player hc::engine hc::io)

configure_file(resources/HEMDigitalConsole.rc.in "${CMAKE_CURRENT_BINARY_DIR}/HEMDigitalConsole.rc" @ONLY)
add_executable(HEMDigitalConsole WIN32 src/main.cpp "${CMAKE_CURRENT_BINARY_DIR}/HEMDigitalConsole.rc")
target_link_libraries(HEMDigitalConsole PRIVATE hem_console_host hc::console_ui hc::media opengl32 shell32 ole32)
target_compile_definitions(HEMDigitalConsole PRIVATE HEM_CONSOLE_VERSION="${HEM_CONSOLE_VERSION}")
hc_copy_assets(HEMDigitalConsole)

add_executable(hem_console_snapshot src/snapshot.cpp)
target_link_libraries(hem_console_snapshot PRIVATE hc::console_ui hc::console_player hc::media hc::io opengl32 shell32 ole32)
hc_copy_assets(hem_console_snapshot)
