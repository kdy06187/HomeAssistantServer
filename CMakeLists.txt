cmake_minimum_required(VERSION 3.10)
project(HomeAssistantCore VERSION 1.0)

# C++17 표준 사용
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 멀티스레드 라이브러리 찾기
find_package(Threads REQUIRED)
# 헤더 파일들이 있는 include 폴더를 경로에 추가
include_directories(include)

# 실행 파일 생성
add_executable(${PROJECT_NAME} 
    src/main.cpp
    src/DeviceManager.cpp
    src/TCPDriver.cpp
)

# SQLite DB와 스레드 라이브러리 연결
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads sqlite3)