# FetchContent_MakeAvailable과 OBJECT 라이브러리 링크(3.12)를 쓰므로 3.14 이상이 필요하다
cmake_minimum_required(VERSION 3.14)
project(HomeAssistantCore VERSION 1.0)

option(HAC_BUILD_TESTS  "단위 테스트(hac_tests) 빌드" ON)
option(HAC_ENABLE_TSAN  "ThreadSanitizer 활성화" OFF)
option(HAC_BUILD_SERVER "Matter/BLE가 필요한 서버 실행 파일 빌드" ON)

# CHIP 정적 라이브러리는 TSan 계측이 안 되어 있어 섞으면 오탐·누락이 생긴다
if(HAC_ENABLE_TSAN AND HAC_BUILD_SERVER)
    message(FATAL_ERROR "HAC_ENABLE_TSAN=ON은 -DHAC_BUILD_SERVER=OFF와 함께 사용하세요.")
endif()

# C++17 표준 사용
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 멀티스레드 라이브러리 찾기
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# 우리 대상에만 경고·TSan 옵션을 건다 (서드파티에는 적용하지 않는다)
function(hac_apply_target_options target)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(HAC_ENABLE_TSAN)
        target_compile_options(${target} PRIVATE -fsanitize=thread -g -O1)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

# ---- hac_core: CHIP·BLE 없이 빌드되는 모듈 ----
add_library(hac_core OBJECT
    src/ApiAuth.cpp
    src/ChipToolOutputParser.cpp
    src/DatabaseManager.cpp
    src/DelayedCommandScheduler.cpp
    src/DeviceManager.cpp
    src/ErrorMapping.cpp
    src/HTTPServer.cpp
    src/RequestValidation.cpp
    src/UtcTime.cpp
    src/PCDriver.cpp
    src/PeriodicTasks.cpp
    src/TcpConnection.cpp
    src/TcpServer.cpp
    src/ProcessChipToolRunner.cpp
    src/MatterController.cpp
)
target_include_directories(hac_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hac_core PUBLIC OpenSSL::Crypto sqlite3 Threads::Threads)
hac_apply_target_options(hac_core)

# ---- 서버 (Matter SDK, SimpleBLE 필요) ----
if(HAC_BUILD_SERVER)
    find_package(simpleble REQUIRED)

    # Matter SDK 경로 설정 (환경에 맞게 수정해야 합니다)
    set(MATTER_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/connectedhomeip") # Matter SDK 최상위 디렉토리
    set(MATTER_OUT_DIR "${MATTER_SDK_DIR}/out/host") # GN/Ninja로 빌드된 결과물(.a 파일들)이 있는 곳
    file(GLOB_RECURSE MATTER_LIBS
        "${MATTER_OUT_DIR}/lib/*.a"
        "${MATTER_OUT_DIR}/obj/*.a"
    )

    # hac_drivers 링크에 GLIB2_LIBRARIES를 쓰므로 타깃 정의보다 먼저 찾는다(Matter BLE 계층이 GIO를 씀)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLIB2 REQUIRED glib-2.0 gio-2.0)

    # CHIP·SimpleBLE에 의존하는 프로토콜 드라이버
    add_library(hac_drivers OBJECT
        src/TCPDriver.cpp
        src/MatterSetupCode.cpp
    )
    target_include_directories(hac_drivers PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${MATTER_SDK_DIR}/src/include
        ${MATTER_SDK_DIR}/src/lib
        ${MATTER_SDK_DIR}/src/controller
        ${MATTER_SDK_DIR}/src
        ${MATTER_SDK_DIR}/zzz_generated/app-common
        ${MATTER_SDK_DIR}/out/host/gen/include
        ${MATTER_SDK_DIR}/third_party/nlassert/repo/include
        ${MATTER_SDK_DIR}/third_party/nlio/repo/include
        ${MATTER_SDK_DIR}/third_party/nlbyteorder/repo/include
        ${MATTER_SDK_DIR}/third_party/inipp/repo/inipp
        ${MATTER_SDK_DIR}/third_party/ot-commissioner/repo/include
    )
    # Matter용 리눅스 환경 매크로 스위치 켜기
    target_compile_definitions(hac_drivers PUBLIC
        CHIP_HAVE_CONFIG_H=1
        CHIP_SYSTEM_CONFIG_USE_SOCKETS=1
        CHIP_SYSTEM_CONFIG_USE_POSIX_SOCKETS=1
    )
    target_link_libraries(hac_drivers PUBLIC
        -Wl,--start-group
        ${MATTER_LIBS}
        ${GLIB2_LIBRARIES}
        -Wl,--end-group
        simpleble
        OpenSSL::SSL
        OpenSSL::Crypto
        event
        event_pthreads
        Threads::Threads
        sqlite3
        # 리눅스/BLE 통신에 필요한 시스템 라이브러리들
        resolv # DNS 해석용
        avahi-client
        avahi-common
        dbus-1
    )
    hac_apply_target_options(hac_drivers)

    add_executable(HomeAssistantCore src/main.cpp)
    target_link_libraries(HomeAssistantCore PRIVATE hac_core hac_drivers)
    hac_apply_target_options(HomeAssistantCore)

endif()

# ---- 테스트 ----
if(HAC_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG 063de7e9578f82b369302001269680b4b1553359 # v1.18.0
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()
