# =============================================================================
#  HEM DIGITAL CONSOLE engines — each piece of the reference rig is its own library:
#    hc::console_common  ConsoleSpecs.hpp (every hardware number, VERIFIED/ASSUMED/UNKNOWN) + Filters.hpp
#    hc::console_desk    HEM STUDIO DESK  (13-channel desk, buses, monitor, bus compressor)
#    hc::console_stepeq  HEM STEP EQ      (2-channel 4-band stepped EQ)
#    hc::console_twinbus HEM TWIN BUS     (2-channel bus compressor + dynamic EQ)
#    hc::console_colour  HEM COLOUR STAGE (stereo colour processor: drive, EQ, HF comp/crush, image, transformer)
#    hc::console_meter   HEM METER BRIDGE (loudness, true peak, stereo image, 1/3-octave RTA, radar history, scope)
#    hc::console_tools   HEM front-end tools (RESO TAMER, SHUFFLE, TAP ECHO) + the ToolSlot behind each INSERT switch
#    hc::console_rig     HEM CONSOLE RIG: the whole rig as one signal path + saved state
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

add_library(hc_console_colour STATIC colour/src/ColourStage.cpp)
add_library(hc::console_colour ALIAS hc_console_colour)
target_include_directories(hc_console_colour PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/colour/include")
target_link_libraries(hc_console_colour PUBLIC hc::console_common)

add_library(hc_console_meter STATIC meter/src/MeterBridge.cpp)
add_library(hc::console_meter ALIAS hc_console_meter)
target_include_directories(hc_console_meter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/meter/include")
target_link_libraries(hc_console_meter PUBLIC hc::console_common)

add_library(hc_console_tools STATIC tools/src/FrontTools.cpp)
add_library(hc::console_tools ALIAS hc_console_tools)
target_include_directories(hc_console_tools PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tools/include")
target_link_libraries(hc_console_tools PUBLIC hc::console_common)

add_library(hc_console_rig STATIC rig/src/ConsoleRig.cpp)
add_library(hc::console_rig ALIAS hc_console_rig)
target_include_directories(hc_console_rig PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/rig/include")
target_link_libraries(hc_console_rig PUBLIC hc::console_desk hc::console_stepeq hc::console_twinbus hc::console_colour
                                            hc::console_meter hc::console_tools)

# The player (engine processor, offline export, session file) and the GPU screen.
add_library(hc_console_player STATIC player/src/ConsolePlayer.cpp)
add_library(hc::console_player ALIAS hc_console_player)
target_include_directories(hc_console_player PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/player/include")
target_link_libraries(hc_console_player PUBLIC hc::console_rig hc::engine hc::media)

add_library(hc_console_ui STATIC ui/src/ConsolePages.cpp ui/src/ConsoleController.cpp ui/src/ConsoleScreen.cpp)
add_library(hc::console_ui ALIAS hc_console_ui)
target_include_directories(hc_console_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ui/include")
target_link_libraries(hc_console_ui PUBLIC hc::console_rig hc::ui)
