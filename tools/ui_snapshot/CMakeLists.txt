add_executable(hc_ui_snapshot main.cpp)
target_link_libraries(hc_ui_snapshot PRIVATE hc::ui hc::io opengl32)
