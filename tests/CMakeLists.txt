add_executable(hac_tests
    unit/ApiAuthTest.cpp
    unit/AuthFailureMonitorTest.cpp
    unit/ErrorMappingTest.cpp
    unit/HttpBindAddressTest.cpp
    unit/RequestValidationTest.cpp
    unit/PCDriverTest.cpp
    unit/ResultTest.cpp
    unit/FakeClockTest.cpp
    unit/DelayedCommandSchedulerTest.cpp
    unit/ChipToolOutputParserTest.cpp
    unit/DatabaseMigrationTest.cpp
    unit/ProcessChipToolRunnerTest.cpp
    integration/DeviceManagerIntegrationTest.cpp
    integration/DeviceManagerBehaviorTest.cpp
    integration/HttpServerTest.cpp
    integration/HttpServerLimitsTest.cpp
    integration/HttpServerLifecycleTest.cpp
    integration/HttpServerAuthTest.cpp
    integration/LockRulesTest.cpp
    integration/EnergyPathTest.cpp
    integration/TcpServerTest.cpp
    integration/TcpServerLifecycleTest.cpp
    integration/MatterControllerTest.cpp
)
# httplib.h(클라이언트)는 src/ 에 있다
target_include_directories(hac_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(hac_tests PRIVATE hac_core GTest::gmock_main)
target_compile_definitions(hac_tests PRIVATE
    HAC_TEST_V0_SCHEMA_SQL="${CMAKE_CURRENT_SOURCE_DIR}/data/v0_schema.sql")
hac_apply_target_options(hac_tests)
gtest_discover_tests(hac_tests)

# 몇 초씩 걸리는 시험은 따로 묶어 라벨 slow를 붙인다(빠르게만 돌릴 때: ctest -LE slow)
add_executable(hac_slow_tests integration/HttpServerSlowTest.cpp)
target_include_directories(hac_slow_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(hac_slow_tests PRIVATE hac_core GTest::gmock_main)
hac_apply_target_options(hac_slow_tests)
gtest_discover_tests(hac_slow_tests PROPERTIES LABELS slow TIMEOUT 120)
