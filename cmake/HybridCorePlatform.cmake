# =============================================================================
#  HybridCorePlatform.cmake
#  Enforces Windows + MSVC + x64 and defines hc::build_flags for the 14700HX.
# =============================================================================
include_guard(GLOBAL)

if(NOT WIN32)
    message(FATAL_ERROR "[HybridCore] Target system must be Windows.")
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR "[HybridCore] MSVC (cl.exe) required. Detected: ${CMAKE_CXX_COMPILER_ID}. "
                        "clang-cl, MinGW and GCC are rejected by design.")
endif()

if(MSVC_VERSION LESS 1938)
    message(FATAL_ERROR "[HybridCore] Visual Studio 2022 17.8 or newer required (MSVC_VERSION=${MSVC_VERSION}).")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "[HybridCore] x64 build required. Use the 'rig' preset or -A x64.")
endif()

# ---- Language ---------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Runtime / LTO ----------------------------------------------------------
# Static CRT: no VC++ redistributable dependency on the rig.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# CMake injects /Ob2 into Release; strip it so /Ob3 below does not trigger D9025.
string(REPLACE "/Ob2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

# ---- Rig flags (interface target, linked by every HybridCore module) --------
add_library(hc_build_flags INTERFACE)
add_library(hc::build_flags ALIAS hc_build_flags)

target_compile_options(hc_build_flags INTERFACE
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /Zc:inline
    /utf-8
    /W4
    /external:anglebrackets
    /external:W0
    /MP
    /arch:AVX2              # Raptor Lake-HX: AVX2 + FMA3. No AVX-512 on this silicon.
    /fp:precise
    /fp:contract            # Allow FMA contraction under precise semantics.
    $<$<CONFIG:Release>:/O2 /Ob3 /Oi /Ot /Gy /Gw /Zi>
    $<$<CONFIG:Debug>:/Od /RTC1 /ZI>
)

target_compile_definitions(hc_build_flags INTERFACE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    STRICT
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00     # Windows 10/11 API surface (IAudioClient3, MMCSS, EfficiencyClass)
    WINVER=0x0A00
    NTDDI_VERSION=0x0A000008   # Win10 2004+: EcoQoS / power-throttling + CPU-set APIs
    # Sample rate, stem count and engine limits live ONLY in core/include/hybrid/core/Config.hpp
    $<$<CONFIG:Release>:NDEBUG>
)

target_link_options(hc_build_flags INTERFACE
    $<$<CONFIG:Release>:/DEBUG:FULL /OPT:REF /OPT:ICF>
)
