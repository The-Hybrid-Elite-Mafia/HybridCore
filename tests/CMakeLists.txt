add_executable(hc_tests
    hc_tests.cpp
    dsp_tests.cpp
    params_tests.cpp
    ui_tests.cpp
    media_tests.cpp
    strip_tests.cpp
    session_tests.cpp
    forge_tests.cpp
    analysis_tests.cpp
    AllocTracker.cpp)
target_include_directories(hc_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/io/src")   # SampleConvert.hpp (internal)
target_link_libraries(hc_tests PRIVATE hc::engine hc::dsp hc::params hc::ui hc::media hc::analysis)
add_test(NAME hc_tests COMMAND hc_tests)
hc_copy_assets(hc_tests)
add_custom_command(TARGET hc_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data" "$<TARGET_FILE_DIR:hc_tests>/testdata"
    VERBATIM)
