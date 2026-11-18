add_executable(hac_tests
    unit/ApiAuthTest.cpp
    unit/PCDriverTest.cpp
    unit/ResultTest.cpp
    unit/FakeClockTest.cpp
    unit/ChipToolOutputParserTest.cpp
    unit/DatabaseMigrationTest.cpp
)
target_include_directories(hac_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hac_tests PRIVATE hac_core GTest::gmock_main)
target_compile_definitions(hac_tests PRIVATE
    HAC_TEST_V0_SCHEMA_SQL="${CMAKE_CURRENT_SOURCE_DIR}/data/v0_schema.sql")
hac_apply_target_options(hac_tests)
gtest_discover_tests(hac_tests)
