# HEM ECHO DECK: engine library. Tests: tests/echo_deck_tests.cpp (built into hc_tests, run by CI).
add_library(hem_echo_deck STATIC src/EchoDeck.cpp)
add_library(hem::echo_deck ALIAS hem_echo_deck)
target_include_directories(hem_echo_deck
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/fx/include")   # Effect interface + DSP helpers (header only, no link cycle)
target_compile_features(hem_echo_deck PUBLIC cxx_std_23)
target_link_libraries(hem_echo_deck PUBLIC hc::build_flags)
