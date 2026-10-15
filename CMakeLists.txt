# =============================================================================
#  THE HYBRID CORE — Root build
#  Target: Windows x64, MSVC only, Intel Core i7-14700HX (Raptor Lake-HX, AVX2)
# =============================================================================
cmake_minimum_required(VERSION 3.28)

# Hard stop before any toolchain is probed. No macOS, no Linux, no cross builds.
if(NOT CMAKE_HOST_WIN32)
    message(FATAL_ERROR "[HybridCore] Windows host required. CoreAudio/ALSA/JACK are not supported by design.")
endif()

# Only the two configs this rig ever ships.
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "HybridCore configurations" FORCE)

# Single version source: the VERSION file (also used by the installer and releases).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" HC_VERSION)
string(STRIP "${HC_VERSION}" HC_VERSION)

project(HybridCore
    VERSION ${HC_VERSION}
    DESCRIPTION "Hybrid Elite Mafia proprietary real-time audio framework"
    LANGUAGES CXX RC)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(HybridCorePlatform)   # MSVC/x64 enforcement + rig compile flags (hc::build_flags)

option(HC_REQUIRE_ASIO_SDK "Fail configuration if the Steinberg ASIO SDK is missing" OFF)
find_package(ASIOSDK)          # cmake/FindASIOSDK.cmake -> hc::asio_sdk when present
if(NOT ASIOSDK_FOUND)
    if(HC_REQUIRE_ASIO_SDK)
        message(FATAL_ERROR "[HybridCore] ASIO SDK required but not found. See external/asiosdk/README.md")
    else()
        message(WARNING "[HybridCore] ASIO SDK not found in external/asiosdk. "
                        "WASAPI path builds; the ASIO host backend will be disabled until the SDK is placed.")
    endif()
endif()

add_subdirectory(core)
add_subdirectory(io)
add_subdirectory(dsp)
add_subdirectory(params)
add_subdirectory(engine)
add_subdirectory(ui)
add_subdirectory(app)
add_subdirectory(tools/device_probe)
add_subdirectory(tools/engine_smoke)
add_subdirectory(tools/ui_snapshot)

enable_testing()
add_subdirectory(tests)

set_property(DIRECTORY ${PROJECT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT HybridCore)
