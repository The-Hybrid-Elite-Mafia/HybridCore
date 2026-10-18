# =============================================================================
#  Optional VST3 builds (HC_BUILD_VST3=ON): the 13-stem matrix, the HEM Suite effects,
#  and the HEM WAVE synth.
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

# HEM Suite: every HEM effect (hc::fx) in one bundle, one plug-in per effect.
# Faceplates (hc::ui, OpenGL) need the Poppins fonts and the console faceplate (knob parts) at run
# time: they ship inside each bundle's Resources folder.
function(hc_add_faceplate_resources target)
    # Copied after the build into <bundle>/Contents/Resources (the SDK's per-file resource rules
    # don't cope with a space in the bundle name under MSBuild).
    get_target_property(pkg ${target} SMTG_PLUGIN_PACKAGE_PATH)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${pkg}/Contents/Resources/fonts" "${pkg}/Contents/Resources/faceplates"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PROJECT_SOURCE_DIR}/assets/fonts/Poppins-Medium.ttf" "${PROJECT_SOURCE_DIR}/assets/fonts/Poppins-SemiBold.ttf"
                "${PROJECT_SOURCE_DIR}/assets/fonts/Poppins-Bold.ttf" "${PROJECT_SOURCE_DIR}/assets/fonts/OFL.txt"
                "${pkg}/Contents/Resources/fonts/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/assets/faceplates/Main-Faceplate.png"
                "${pkg}/Contents/Resources/faceplates/"
        VERBATIM)
endfunction()

smtg_add_vst3plugin(HemSuite PACKAGE_NAME "HEM Suite" SOURCES_LIST src/HemSuiteVst3.cpp
    "${vst3sdk_SOURCE_DIR}/public.sdk/source/common/pluginview.cpp")
set_target_properties(HemSuite PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(HemSuite SYSTEM PRIVATE "${vst3sdk_SOURCE_DIR}")
target_compile_definitions(HemSuite PRIVATE HC_VERSION_STR="${PROJECT_VERSION}")
target_link_libraries(HemSuite PRIVATE sdk hc::fx hc::ui)
hc_add_faceplate_resources(HemSuite)

# HEM WAVE: the wavetable synth (hc::synth) as a VST3 instrument.
smtg_add_vst3plugin(HemWave PACKAGE_NAME "HEM WAVE" SOURCES_LIST src/HemWaveVst3.cpp
    "${vst3sdk_SOURCE_DIR}/public.sdk/source/common/pluginview.cpp")
set_target_properties(HemWave PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(HemWave SYSTEM PRIVATE "${vst3sdk_SOURCE_DIR}")
target_compile_definitions(HemWave PRIVATE HC_VERSION_STR="${PROJECT_VERSION}")
target_link_libraries(HemWave PRIVATE sdk hc::synth hc::ui)
hc_add_faceplate_resources(HemWave)

message(STATUS "[HybridCore] VST3 plug-in enabled (SDK ${vst3sdk_SOURCE_DIR})")
