# HEM COMPRESSOR: engine library. Tests: tests/compressor_tests.cpp (built into hc_tests, run by CI).
add_library(hem_compressor STATIC src/Compressor.cpp)
add_library(hem::compressor ALIAS hem_compressor)
target_include_directories(hem_compressor
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/fx/include")   # Effect interface + DSP helpers (header only, no link cycle)
target_compile_features(hem_compressor PUBLIC cxx_std_23)
target_link_libraries(hem_compressor PUBLIC hc::build_flags)
