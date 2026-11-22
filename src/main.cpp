#include <iostream>
#include <chrono>
#include <sqlite3.h>
#include "DeviceManager.hpp"
#include "DatabaseManager.hpp"
#include "TCPDriver.hpp"
#include "MatterController.hpp"
#include "ProcessChipToolRunner.hpp"
#include "HTTPServer.hpp"
#include "RequestValidation.hpp"
#include "DriverManager.hpp"
#include "PCDriver.hpp"
#include "IMessageSender.hpp"
#include "ApiAuth.hpp"
#include "PeriodicTasks.hpp"
#include "Clock.hpp"
#include "DelayedCommandScheduler.hpp"
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>  // umask
int main() {
    // 어떤 스레드보다 먼저 막아야 이후 만드는 모든 스레드(와 chip-tool 자식 준비 단계)가 마스크를 물려받아,
    // 종료 신호가 main의 sigwait 한 곳으로만 모인다
    sigset_t terminationSignals;
    sigemptyset(&terminationSignals);
    sigaddset(&terminationSignals, SIGTERM);
    sigaddset(&terminationSignals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &terminationSignals, nullptr);

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
    const auto httpBindAddress = RequestValidation::parseBindAddress(std::getenv("HAC_HTTP_BIND"));
    if (!httpBindAddress.ok()) {
        std::cerr << "❌ 환경 변수 HAC_HTTP_BIND 형식 오류(IPv4 주소 필요). 서버를 시작하지 않습니다." << std::endl;
        return 1;
    }

    // HTTP와 같은 방식으로 TCP 바인딩 주소를 읽는다(기기 접속 포트 8080)
    const auto tcpBindAddress = RequestValidation::parseBindAddress(std::getenv("HAC_TCP_BIND"));
    if (!tcpBindAddress.ok()) {
        std::cerr << "❌ 환경 변수 HAC_TCP_BIND 형식 오류(IPv4 주소 필요). 서버를 시작하지 않습니다." << std::endl;
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
    // 선언의 역순으로 소멸한다: httpServer → periodicTasks → 드라이버 → manager → scheduler → database.
    // 객체를 쓰는 쪽이 먼저 사라져야 하므로 쓰이는 쪽(database, scheduler, manager)을 먼저 선언한다
    DriverManager deviceDrivers;
    SystemClock clock;
    DelayedCommandScheduler scheduler(clock);
    DeviceManager manager(database, deviceDrivers, scheduler, clock);
    scheduler.start();
    // 2. TCP 드라이버 생성 및 포트 8080으로 서버 시작
    TCPDriver tcpDriver;
    // 포트를 못 열면(이전 프로세스가 남아 있는 등) 반쯤 켜진 채 돌지 않고 바로 끝낸다
    if (!tcpDriver.startServer(8080, *tcpBindAddress.value)) {
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
    // 어떤 프로토콜의 기기든 PC 종료 메시지는 TCP 드라이버로 보낸다
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [&tcpDriver](IMessageSender*) {
        return std::make_shared<PCDriver>(&tcpDriver);
    });
    manager.registerDriver(ProtocolType::MATTER, &matterController);
    
    PeriodicTasks periodicTasks(manager);
    periodicTasks.start();
    manager.initFromDatabase();
    HTTPServer httpServer(manager, *apiToken, 8000, *httpBindAddress.value);
    // start()가 바인딩까지 마치므로 실패하면 바로 알 수 있다. 수락 스레드가 listen에 들어갈 때까지 한 번 더 확인한다
    if (!httpServer.start() || !httpServer.waitUntilReady(std::chrono::seconds(5))) {
        std::cerr << "❌ HTTP 포트 8000을 열 수 없습니다(원인은 위 [HTTPServer] 로그). 서버를 종료합니다." << std::endl;
        return 1;
    }


    //  관리 대상 기기 임시 등록
    // manager.addDevice("Arduino_1", "거실 전등", ProtocolType::TCP_DIY);
    // manager.addDevice("1", "스마트 플러그", ProtocolType::MATTER);

    // 종료 신호가 올 때까지 대기한다
    int signalNumber = 0;
    sigwait(&terminationSignals, &signalNumber);
    // 종료가 늦어질 때 한 번 더 보내면 기본 동작으로 즉시 끝나도록, main 스레드만 마스크를 푼다
    pthread_sigmask(SIG_UNBLOCK, &terminationSignals, nullptr);
    std::cout << "[Main] 종료 신호 수신(" << (signalNumber == SIGTERM ? "SIGTERM" : "SIGINT")
              << "), 종료를 시작합니다" << std::endl;

    // 0단계: 진행 중인 chip-tool을 끊고, 대기 중인 지연 명령을 취소한다.
    // 이 단계가 HTTP 정지보다 먼저여야 종료 중에 45초가 만기되어 PC 전원이 꺼지는 일이 없다
    manager.beginShutdown();
    chipToolRunner.cancelAll();
    scheduler.stopAndCancelAll();
    periodicTasks.stop();
    httpServer.stop();
    matterController.shutdown();
    tcpDriver.stopServer();
    database.close();
    std::cout << "[Main] 종료 완료" << std::endl;
    return 0;
}
