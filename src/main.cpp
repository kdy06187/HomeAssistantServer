#include <iostream>
#include <thread>
#include <chrono>
#include <sqlite3.h>
#include "DeviceManager.hpp"
#include "DatabaseManager.hpp"
#include "TCPDriver.hpp"
#include "MatterController.hpp"
#include "ProcessChipToolRunner.hpp"
#include "HTTPServer.hpp"
#include "DriverManager.hpp"
#include "PCDriver.hpp"
#include "IMessageSender.hpp"
#include "ApiAuth.hpp"
#include "PeriodicTasks.hpp"
#include "Clock.hpp"
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>  // umask
int main() {
    // 서버와 자식 chip-tool이 만드는 파일(DB·등록 파일·/tmp의 chip-tool 저장소)을 소유자만 읽게 한다
    umask(077);
    signal(SIGPIPE, SIG_IGN);
    std::cout << "=== 🏠 홈 어시스턴트 코어 부팅 ===" << std::endl;

    // 기기 드라이버를 띄우기 전에 확인해, 토큰이 없으면 아무것도 시작하지 않는다.
    const auto apiToken = ApiAuth::loadToken("../config/api_token");
    if (!apiToken) {
        std::cerr << "❌ API 토큰 로드 실패! 서버를 시작하지 않습니다." << std::endl;
        return 1;
    }

    // 잘못된 주소로 조용히 기본값(모든 NIC)에 열리지 않도록, 형식 오류는 시작 실패로 처리한다.
    // 로그에는 변수 이름만 남긴다.
    const auto httpBindAddress = parseHttpBindAddress(std::getenv("HAC_HTTP_BIND"));
    if (!httpBindAddress.ok()) {
        std::cerr << "❌ 환경 변수 HAC_HTTP_BIND 형식 오류(IPv4 주소 필요). 서버를 시작하지 않습니다." << std::endl;
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
    // 포트를 못 열면(이전 프로세스가 남아 있는 등) 반쯤 켜진 채 돌지 않고 바로 끝낸다
    if (!tcpDriver.startServer(8080)) {
        std::cerr << "❌ TCP 포트 8080을 열 수 없습니다(원인은 위 [TCPDriver] 로그). 서버를 종료합니다." << std::endl;
        return 1;
    }
    
    // 3. DeviceManager에 드라이버 등록
    manager.registerDriver(ProtocolType::TCP_DIY, &tcpDriver);
    
    // MatterController 생성 및 초기화
    ProcessChipToolRunner chipToolRunner;  // chip-tool을 셸 없이 실행한다
    MatterController matterController(chipToolRunner, parseManualSetupPin);
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
    HTTPServer httpServer(manager, *apiToken, 8000, *httpBindAddress.value);
    httpServer.start();
    // start()는 스레드만 띄운다. listen이 실패하면 run()이 바로 끝나므로 준비 여부로 판단한다
    if (!httpServer.waitUntilReady(std::chrono::seconds(5))) {
        std::cerr << "❌ HTTP 포트 8000을 열 수 없습니다(원인은 위 [HTTPServer] 로그). 서버를 종료합니다." << std::endl;
        return 1;
    }


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
