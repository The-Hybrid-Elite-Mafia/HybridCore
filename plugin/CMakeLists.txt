# =============================================================================
#  Optional VST3 build of the 13-stem matrix (HC_BUILD_VST3=ON).
#  Steinberg VST3 SDK v3.8.1 (MIT licence) is fetched at configure time — never
#  committed. VSTGUI and the SDK examples are not fetched/built. Steinberg's
#  validator is built and run automatically after the plug-in links.
# =============================================================================
include(FetchContent)

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT        OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK           OFF CACHE BOOL "" FORCE)   # no symlink into Common Files (needs admin)
set(SMTG_RUN_VST_VALIDATOR            ON  CACHE BOOL "" FORCE)
set(SMTG_USE_STATIC_CRT               ON  CACHE BOOL "" FORCE)   # matches HybridCore's /MT

# The SDK is C++17 code: build it as C++17 (this scope only); our plug-in target stays C++23.
set(CMAKE_CXX_STANDARD 17)

FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG        v3.8.1_build_84
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES base cmake pluginterfaces public.sdk
    GIT_PROGRESS   FALSE)
FetchContent_MakeAvailable(vst3sdk)

smtg_add_vst3plugin(HybridCoreVST3 PACKAGE_NAME "The Hybrid Core" SOURCES_LIST src/HybridCoreVst3.cpp)
set_target_properties(HybridCoreVST3 PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(HybridCoreVST3 SYSTEM PRIVATE "${vst3sdk_SOURCE_DIR}")
target_compile_definitions(HybridCoreVST3 PRIVATE HC_VERSION_STR="${PROJECT_VERSION}")
target_link_libraries(HybridCoreVST3 PRIVATE sdk hc::params hc::dsp)

message(STATUS "[HybridCore] VST3 plug-in enabled (SDK ${vst3sdk_SOURCE_DIR})")
