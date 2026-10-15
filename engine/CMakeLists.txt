# =============================================================================
#  hc::engine — lock-free audio callback, command/telemetry rings, processors
# =============================================================================
add_library(hc_engine STATIC
    src/Engine.cpp
    src/ToneProcessor.cpp)
add_library(hc::engine ALIAS hc_engine)

target_include_directories(hc_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(hc_engine PUBLIC hc::build_flags hc::core hc::io)
