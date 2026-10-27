# =============================================================================
#  HEM DIGITAL CONSOLE engines — each piece of the reference rig is its own library:
#    hc::console_common  ConsoleSpecs.hpp (every hardware number, VERIFIED/ASSUMED/UNKNOWN) + Filters.hpp
#    hc::console_desk    HEM STUDIO DESK  (13-channel desk, buses, monitor, bus compressor)
#    hc::console_stepeq  HEM STEP EQ      (2-channel 4-band stepped EQ)
#    hc::console_twinbus HEM TWIN BUS     (2-channel bus compressor + dynamic EQ)
#  None of them touch hc::fx's existing effects (HEM DESK, HEM BUS PRESS ...); they only reuse its
#  Effect interface and small filter helpers.
# =============================================================================
add_library(hc_console_common INTERFACE)
add_library(hc::console_common ALIAS hc_console_common)
target_include_directories(hc_console_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/common/include")
target_link_libraries(hc_console_common INTERFACE hc::build_flags hc::core hc::fx)

add_library(hc_console_desk STATIC desk/src/StudioDesk.cpp)
add_library(hc::console_desk ALIAS hc_console_desk)
target_include_directories(hc_console_desk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/desk/include")
target_link_libraries(hc_console_desk PUBLIC hc::console_common)

add_library(hc_console_stepeq STATIC stepeq/src/StepEq.cpp)
add_library(hc::console_stepeq ALIAS hc_console_stepeq)
target_include_directories(hc_console_stepeq PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stepeq/include")
target_link_libraries(hc_console_stepeq PUBLIC hc::console_common)

add_library(hc_console_twinbus STATIC twinbus/src/TwinBus.cpp)
add_library(hc::console_twinbus ALIAS hc_console_twinbus)
target_include_directories(hc_console_twinbus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/twinbus/include")
target_link_libraries(hc_console_twinbus PUBLIC hc::console_common)
