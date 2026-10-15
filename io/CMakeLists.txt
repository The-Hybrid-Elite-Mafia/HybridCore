# =============================================================================
#  hc::io — Windows audio hardware layer (WASAPI + ASIO). No other backends.
# =============================================================================
add_library(hc_io STATIC
    src/ComApartment.cpp
    src/WasapiEnumerator.cpp
    src/AsioRegistry.cpp
    src/CpuProbe.cpp
    src/SampleConvert.cpp
    src/WasapiExclusiveOutput.cpp
    src/OfflineStream.cpp)
add_library(hc::io ALIAS hc_io)

target_include_directories(hc_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(hc_io
    PUBLIC  hc::build_flags hc::core
    PRIVATE ole32 advapi32)

if(TARGET hc::asio_sdk)
    target_sources(hc_io PRIVATE src/AsioDuplex.cpp)
    target_link_libraries(hc_io PUBLIC hc::asio_sdk)
endif()
