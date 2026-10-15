add_executable(hc_ui_snapshot main.cpp)
target_link_libraries(hc_ui_snapshot PRIVATE hc_apphost hc::params hc::ui opengl32)
hc_copy_assets(hc_ui_snapshot)
