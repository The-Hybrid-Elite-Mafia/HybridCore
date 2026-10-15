# =============================================================================
#  FindASIOSDK.cmake
#  Locates the Steinberg ASIO SDK under external/asiosdk (or HC_ASIO_SDK_DIR)
#  and builds it as hc::asio_sdk. The SDK is not redistributable: it is never
#  committed, only discovered.
# =============================================================================
include_guard(GLOBAL)

set(HC_ASIO_SDK_DIR "${PROJECT_SOURCE_DIR}/external/asiosdk" CACHE PATH "Steinberg ASIO SDK location")

# Accept the SDK extracted directly or inside its versioned folder.
file(GLOB _hc_asio_candidates LIST_DIRECTORIES true "${HC_ASIO_SDK_DIR}/*")

find_path(ASIOSDK_ROOT
    NAMES common/asio.h
    PATHS "${HC_ASIO_SDK_DIR}" ${_hc_asio_candidates}
    NO_DEFAULT_PATH
    NO_CACHE)

set(_hc_asio_required
    common/asio.h
    common/asio.cpp
    common/asiosys.h
    common/iasiodrv.h
    host/asiodrivers.h
    host/asiodrivers.cpp
    host/pc/asiolist.h
    host/pc/asiolist.cpp)

if(ASIOSDK_ROOT)
    foreach(_f IN LISTS _hc_asio_required)
        if(NOT EXISTS "${ASIOSDK_ROOT}/${_f}")
            message(WARNING "[HybridCore] ASIO SDK at ${ASIOSDK_ROOT} is missing ${_f}")
            unset(ASIOSDK_ROOT)
            break()
        endif()
    endforeach()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ASIOSDK REQUIRED_VARS ASIOSDK_ROOT)

if(ASIOSDK_FOUND AND NOT TARGET hc_asio_sdk)
    add_library(hc_asio_sdk STATIC
        "${ASIOSDK_ROOT}/common/asio.cpp"
        "${ASIOSDK_ROOT}/host/asiodrivers.cpp"
        "${ASIOSDK_ROOT}/host/pc/asiolist.cpp")
    add_library(hc::asio_sdk ALIAS hc_asio_sdk)

    target_include_directories(hc_asio_sdk SYSTEM PUBLIC
        "${ASIOSDK_ROOT}/common"
        "${ASIOSDK_ROOT}/host"
        "${ASIOSDK_ROOT}/host/pc")

    # Legacy Steinberg sources: ANSI registry calls, pre-C++11 idioms.
    # Isolated from hc::build_flags (no UNICODE, no /permissive-, no /W4).
    set_target_properties(hc_asio_sdk PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(hc_asio_sdk PRIVATE /permissive /W0 /arch:AVX2 /utf-8)
    target_compile_definitions(hc_asio_sdk
        PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX
        PUBLIC  HC_HAS_ASIO_SDK=1)
    target_link_libraries(hc_asio_sdk PUBLIC ole32 advapi32 user32 uuid)

    message(STATUS "[HybridCore] ASIO SDK: ${ASIOSDK_ROOT}")
endif()
