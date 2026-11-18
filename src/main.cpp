#include <iostream>
#include <thread>
#include <chrono>
#include <sqlite3.h>
#include "DeviceManager.hpp"
#include "DatabaseManager.hpp"
#include "TCPDriver.hpp"
#include "MatterController.hpp"
#include "HTTPServer.hpp"
#include "DriverManager.hpp"
#include "PCDriver.hpp"
#include "IMessageSender.hpp"
#include "ApiAuth.hpp"
#include "PeriodicTasks.hpp"
#include "Clock.hpp"
#include <csignal>
#include <unistd.h>
int main() {
    signal(SIGPIPE, SIG_IGN);
    std::cout << "=== 🏠 홈 어시스턴트 코어 부팅 ===" << std::endl;

    // 기기 드라이버를 띄우기 전에 확인해, 토큰이 없으면 아무것도 시작하지 않는다.
    const auto apiToken = ApiAuth::loadToken("../config/api_token");
    if (!apiToken) {
        std::cerr << "❌ API 토큰 로드 실패! 서버를 시작하지 않습니다." << std::endl;
        return 1;
    }

    // 0. 데이터베이스 준비
    DatabaseManager database;
    if (!database.init("../db/home_assistant.db")) {
        std::cerr << "❌ DB 초기화 실패!" << std::endl;
        return 1;
    }
    std::cout << "✅ 데이터베이스 로드 완료!" << std::endl;
    // 1. 시스템 두뇌(DeviceManager) 조립
    DriverManager deviceDrivers;
    SystemClock clock;
    DeviceManager manager(database, deviceDrivers, clock);
    // 2. TCP 드라이버 생성 및 포트 8080으로 서버 시작
    TCPDriver tcpDriver;
    tcpDriver.startServer(8080);
    
    // 3. DeviceManager에 드라이버 등록
    manager.registerDriver(ProtocolType::TCP_DIY, &tcpDriver);
    
    // MatterController 생성 및 초기화
    MatterController matterController;
    if (matterController.Initialize()) {
        std::cout << "[Main] MatterController 초기화 및 스레드 구동 성공!" << std::endl;
    } else {
        std::cerr << "[Main] MatterController 초기화 실패!" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    // 어떤 프로토콜의 기기든 PC 종료 메시지는 TCP 드라이버로 보낸다
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [&tcpDriver](IMessageSender*) {
        return std::make_shared<PCDriver>(&tcpDriver);
    });
    manager.registerDriver(ProtocolType::MATTER, &matterController);
    
    PeriodicTasks periodicTasks(manager);
    periodicTasks.start();
    manager.initFromDatabase();
    HTTPServer httpServer(manager, *apiToken, 8000);
    httpServer.start();


    //  관리 대상 기기 임시 등록
    // manager.addDevice("Arduino_1", "거실 전등", ProtocolType::TCP_DIY);
    // manager.addDevice("1", "스마트 플러그", ProtocolType::MATTER);

    // 프로세스는 kill로 종료한다. 아래 종료 코드는 도달하지 않는다.
    for (;;) pause();

    httpServer.stop();
    periodicTasks.stop();
    matterController.shutdown();
    tcpDriver.stopServer();
    return 0;
}
