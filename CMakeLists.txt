cmake_minimum_required(VERSION 3.10)
project(HomeAssistantCore VERSION 1.0)

# C++17 표준 사용
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 멀티스레드 라이브러리 찾기
find_package(Threads REQUIRED)

find_package(OpenSSL REQUIRED)


# Matter SDK 경로 설정 (⚠️ 주의: 실제 학생님의 환경에 맞게 수정해야 합니다)
set(MATTER_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/connectedhomeip") # Matter SDK 최상위 디렉토리
set(MATTER_OUT_DIR "${MATTER_SDK_DIR}/out/host") # GN/Ninja로 빌드된 결과물(.a 파일들)이 있는 곳
file(GLOB_RECURSE MATTER_LIBS 
    "${MATTER_OUT_DIR}/lib/*.a" 
    "${MATTER_OUT_DIR}/obj/*.a"
)
# message(FATAL_ERROR "🔥 찾은 Matter 라이브러리 개수 및 목록: ${MATTER_LIBS}")
# 헤더 파일들이 있는 include 폴더를 경로에 추가
include_directories(include)

include_directories(
    ${MATTER_SDK_DIR}/src/include
    ${MATTER_SDK_DIR}/src/lib
    ${MATTER_SDK_DIR}/src/controller
    ${MATTER_SDK_DIR}/src
)
# 소스 파일 및 실행 파일(Target) 생성

set(SOURCES
    src/main.cpp
    src/MatterController.cpp
    # src/HTTPServer.cpp (추후 추가)
    # src/TCPServer.cpp (추후 추가)
)

# 실행 파일 생성
add_executable(${PROJECT_NAME} 
    src/main.cpp
    src/DeviceManager.cpp
    src/TCPDriver.cpp
    src/MatterController.cpp
)
target_include_directories(HomeAssistantCore PRIVATE
    ${GLIB2_INCLUDE_DIRS}
)
target_link_libraries(${PROJECT_NAME} PRIVATE
    -Wl,--start-group
    ${MATTER_LIBS}
    ${GLIB2_LIBRARIES}
    -Wl,--end-group
)
# SQLite DB와 스레드 라이브러리 연결
target_link_libraries(${PROJECT_NAME} PRIVATE 
    OpenSSL::SSL
    OpenSSL::Crypto    
    event
    event_pthreads
    Threads::Threads 
    sqlite3
    # Matter SDK에서 뽑아낸 정적 라이브러리들 (.a)
    # (참고: 빌드 옵션에 따라 라이브러리 이름이 CHIP, CHIPController 등으로 다를 수 있습니다)
    # ${MATTER_OUT_DIR}/libCHIP.a

    # 리눅스/BLE 통신에 필요한 시스템 라이브러리들
    resolv # DNS 해석용
    avahi-client
    avahi-common
    dbus-1
    
)

# Matter용 리눅스 환경 매크로 스위치 켜기
target_compile_definitions(HomeAssistantCore PRIVATE
    CHIP_HAVE_CONFIG_H=1
    CHIP_SYSTEM_CONFIG_USE_SOCKETS=1
    CHIP_SYSTEM_CONFIG_USE_POSIX_SOCKETS=1
)
# 2. Matter SDK 헤더 파일 탐색 경로 (Include Directories)
# =======================================================
target_include_directories(HomeAssistantCore PRIVATE
    ${MATTER_SDK_DIR}/src
    ${MATTER_SDK_DIR}/src/include
    
    
    
    ${MATTER_SDK_DIR}/zzz_generated/app-common
    ${MATTER_SDK_DIR}/out/host/gen/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include

    ${MATTER_SDK_DIR}/third_party/nlassert/repo/include
    ${MATTER_SDK_DIR}/third_party/nlio/repo/include
    ${MATTER_SDK_DIR}/third_party/nlbyteorder/repo/include
    ${MATTER_SDK_DIR}/third_party/inipp/repo/inipp
    ${MATTER_SDK_DIR}/third_party/ot-commissioner/repo/include
)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB2 REQUIRED glib-2.0 gio-2.0)# Matter Device Layer에서 종종 사용함
