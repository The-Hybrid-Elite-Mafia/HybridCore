# =============================================================================
#  hc::hiss — HEM Hiss Tamer (hem_hiss_tamer_engine) + HEM Hiss Auto (hem_hiss_auto)
#  Pure C++, no JUCE. Needs nothing else from The Hybrid Core, so it also builds on its own:
#      cmake -S hiss -B build/hiss -G "Visual Studio 17 2022" -A x64
# =============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(HemHiss LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
    include(HybridCorePlatform)
    enable_testing()
endif()

add_library(hc_hiss STATIC
    src/HissTamerEngine.cpp
    src/HissMetrics.cpp
    src/HissAuto.cpp)
add_library(hc::hiss ALIAS hc_hiss)
target_include_directories(hc_hiss PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hc_hiss PUBLIC hc::build_flags)

option(HC_BUILD_HISS_TOOLS "Build the Hiss command-line tool and its tests" ON)
if(HC_BUILD_HISS_TOOLS)
  # Command-line tool: one command fixes a file, a folder of stems, or runs the built-in demo scenes.
  add_executable(hc_hiss_cli tools/hiss_cli.cpp)
  target_include_directories(hc_hiss_cli PRIVATE tools tests)
  target_link_libraries(hc_hiss_cli PRIVATE hc::hiss)

  # Tests (their own small harness + a heap-allocation counter to prove the audio path never allocates)
  add_executable(hc_hiss_tests tests/hiss_tests.cpp tests/HissAllocTracker.cpp)
  target_include_directories(hc_hiss_tests PRIVATE tests)
  target_link_libraries(hc_hiss_tests PRIVATE hc::hiss)
  add_test(NAME hiss_tests COMMAND hc_hiss_tests)
endif()
