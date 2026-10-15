# =============================================================================
#  hc::core — lock-free primitives, aligned memory, real-time thread policy
# =============================================================================
add_library(hc_core STATIC
    src/RealtimeThread.cpp)
add_library(hc::core ALIAS hc_core)

target_include_directories(hc_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(hc_core
    PUBLIC  hc::build_flags
    PRIVATE avrt)
