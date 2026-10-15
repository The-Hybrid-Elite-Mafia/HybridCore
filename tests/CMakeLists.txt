add_executable(hc_tests
    hc_tests.cpp
    dsp_tests.cpp
    AllocTracker.cpp)
target_include_directories(hc_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/io/src")   # SampleConvert.hpp (internal)
target_link_libraries(hc_tests PRIVATE hc::engine hc::dsp)
add_test(NAME hc_tests COMMAND hc_tests)
