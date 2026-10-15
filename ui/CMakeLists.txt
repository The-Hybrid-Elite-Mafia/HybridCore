# =============================================================================
#  hc::ui — GPU UI: image pipeline, faceplates, layout, font, OpenGL 3.3 renderer
# =============================================================================
add_library(hc_ui STATIC
    src/StbImpl.cpp
    src/Image.cpp
    src/Theme.cpp
    src/Layout.cpp
    src/FontAtlas.cpp
    src/Gl.cpp
    src/GlWindow.cpp
    src/Renderer.cpp
    src/MixerView.cpp)
add_library(hc::ui ALIAS hc_ui)

target_include_directories(hc_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Vendored stb headers are third-party: SYSTEM so /external:W0 silences their warnings.
target_include_directories(hc_ui SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/external/stb")
target_link_libraries(hc_ui
    PUBLIC  hc::build_flags hc::core hc::params
    PRIVATE opengl32 user32 gdi32)
