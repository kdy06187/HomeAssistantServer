add_executable(hac_tests
    unit/ApiAuthTest.cpp
    unit/PCDriverTest.cpp
    unit/ResultTest.cpp
    unit/FakeClockTest.cpp
    unit/ChipToolOutputParserTest.cpp
    unit/DatabaseMigrationTest.cpp
    integration/DeviceManagerIntegrationTest.cpp
    integration/DeviceManagerBehaviorTest.cpp
    integration/HttpServerTest.cpp
    integration/LockRulesTest.cpp
    integration/EnergyPathTest.cpp
)
# httplib.h(클라이언트)는 src/ 에 있다
target_include_directories(hac_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(hac_tests PRIVATE hac_core GTest::gmock_main)
target_compile_definitions(hac_tests PRIVATE
    HAC_TEST_V0_SCHEMA_SQL="${CMAKE_CURRENT_SOURCE_DIR}/data/v0_schema.sql")
hac_apply_target_options(hac_tests)
gtest_discover_tests(hac_tests)
