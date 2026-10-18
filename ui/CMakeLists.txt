# =============================================================================
#  hc::ui — GPU UI: faceplate chassis, parts atlas, Poppins, layout, OpenGL 3.3 renderer
# =============================================================================
add_library(hc_ui STATIC
    src/StbImpl.cpp
    src/Image.cpp
    src/Faceplate.cpp
    src/Parts.cpp
    src/Layout.cpp
    src/FontAtlas.cpp
    src/Gl.cpp
    src/GlWindow.cpp
    src/Renderer.cpp
    src/MixerView.cpp
    src/ScopeView.cpp
    src/Console.cpp
    src/FxPanel.cpp
    src/FxPanelSpec.cpp
    src/FxPanelView.cpp
    src/FxPanelWindow.cpp)
add_library(hc::ui ALIAS hc_ui)

target_include_directories(hc_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Vendored stb headers are third-party: SYSTEM so /external:W0 silences their warnings.
target_include_directories(hc_ui SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/external/stb")
target_link_libraries(hc_ui
    PUBLIC  hc::build_flags hc::core hc::params hc::forge hc::analysis hc::fx
    PRIVATE opengl32 user32 gdi32 shell32)
