add_executable(hc_tests hc_tests.cpp)
target_include_directories(hc_tests PRIVATE "${PROJECT_SOURCE_DIR}/io/src")   # SampleConvert.hpp (internal)
target_link_libraries(hc_tests PRIVATE hc::engine)
add_test(NAME hc_tests COMMAND hc_tests)
