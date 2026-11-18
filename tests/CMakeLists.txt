add_executable(hac_tests
    unit/ApiAuthTest.cpp
    unit/PCDriverTest.cpp
    unit/ResultTest.cpp
    unit/FakeClockTest.cpp
    unit/ChipToolOutputParserTest.cpp
)
target_include_directories(hac_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hac_tests PRIVATE hac_core GTest::gmock_main)
hac_apply_target_options(hac_tests)
gtest_discover_tests(hac_tests)
