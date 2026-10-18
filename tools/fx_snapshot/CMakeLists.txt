add_executable(hc_fx_snapshot main.cpp)
target_link_libraries(hc_fx_snapshot PRIVATE hc::ui hc::fx hc::synth opengl32)
hc_copy_assets(hc_fx_snapshot)
