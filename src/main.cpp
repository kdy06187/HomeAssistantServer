#include <iostream>
#include <thread>
#include <chrono>
#include <sqlite3.h>
#include "DeviceManager.hpp"
#include "DatabaseManager.hpp"
#include "TCPDriver.hpp"
#include "MatterController.hpp"
#include "HTTPServer.hpp"

int main() {
    std::cout << "=== 🏠 홈 어시스턴트 코어 부팅 ===" << std::endl;

    // 0. 데이터베이스 준비
    if (!DatabaseManager::getInstance().init("../db/home_assistant.db")) {
        std::cerr << "❌ DB 초기화 실패!" << std::endl;
        return 1;
    }
    std::cout << "✅ 데이터베이스 로드 완료!" << std::endl;

    // 1. 시스템 두뇌(DeviceManager) 가져오기
    DeviceManager& manager = DeviceManager::getInstance();
    manager.initFromDatabase();
    manager.startHealthCheck();
    // 2. TCP 드라이버 생성 및 포트 8080으로 서버 시작
    TCPDriver* tcpDriver = new TCPDriver(manager);
    tcpDriver->startServer(8080);

    // 3. DeviceManager에 드라이버 등록
    manager.registerDriver(ProtocolType::TCP_DIY, tcpDriver);

    // MatterController 생성 및 초기화
    MatterController* matterController = new MatterController(manager);
    if (matterController->Initialize()) {
        std::cout << "[Main] MatterController 초기화 및 스레드 구동 성공!" << std::endl;
    } else {
        std::cerr << "[Main] MatterController 초기화 실패!" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    manager.registerDriver(ProtocolType::MATTER, matterController);

    HTTPServer httpServer(manager, 8000);
    httpServer.start();
    //  관리 대상 기기 임시 등록
    // manager.addDevice("Arduino_1", "거실 전등", ProtocolType::TCP_DIY);
    // manager.addDevice("1", "스마트 플러그", ProtocolType::MATTER);

    // uint64_t targetNodeId = 1;
    // std::string setupPinCode = "34460414140"; //실제 기기 핀 코드

    // std::cout << "\n🔗 [Matter] 상용 기기 커미셔닝(페어링) 프로세스 시작..." << std::endl;
    // bool commissionSuccess = matterController->commissionDevice(targetNodeId, setupPinCode,
    //                                                     "U+Net8683", "38835318M#");

    // if (commissionSuccess) {
    //     std::cout << "🚀 커미셔닝 요청 성공! 로컬 네트워크상에서 기기와 보안 세션(PASE) 협상을 진행합니다." << std::endl;
    // } else {
    //     std::cerr << "⚠️ 커미셔닝 요청에 실패했거나 이미 등록된 기기일 수 있습니다." << std::endl;
    // };

    // 통신 세션 안정화를 위해 잠시 대기 (약 5초)
    std::cout << "⏳ 기기 네트워크 안정화 대기 중 (5초)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    //서버 메인 루프 (프로그램이 꺼지지 않게 무한 대기하며 10초마다 명령 테스트)
    bool isTurnedOn = false;
    int consecutiveFailures = 0;      // 연속 실패 횟수 추적
    const int MAX_FAILURES = 3;       // 치명적 장애로 판단할 임계치
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        // isTurnedOn = !isTurnedOn;
        // std::string command = isTurnedOn ? "TURN_ON" : "TURN_OFF";
        std::cout << "\n[Main] 시스템 테스트: 기기에 제어 명령 대기!" << std::endl;
        // manager.executeCommand("Arduino_1", command);

        // bool success = manager.executeCommand("1", command);
        
        // if (success) {
        //     consecutiveFailures = 0; // 성공 시 실패 카운터 즉시 리셋
        //     std::cout << "🟢 [System] 명령 전송 성공! 기기가 정상 응답했습니다." << std::endl;
        // } else {
        //     consecutiveFailures++;
        //     std::cerr << "🔴 [System] 기기 응답 없음! 연속 실패: " << consecutiveFailures << "/" << MAX_FAILURES << std::endl;
            
        //     // 임계치(3회) 도달 시 자가 치유(Self-Healing) 발동
        //     if (consecutiveFailures >= MAX_FAILURES) {
        //         std::cerr << "🚨 [System] 치명적 장애 감지! 해당 기기를 시스템에서 분리합니다." << std::endl;
                
        //         // 파일 및 chip-tool 내부 캐시에서 기기 완전 삭제
        //         matterController->removeDeviceRegistration(targetNodeId);
                
        //         std::cout << "📱 [System] 모바일 앱으로 [연결 끊김 및 재등록 필요] 알림 푸시 전송 (예정)" << std::endl;
                
        //         // 루프를 탈출하여 무의미한 명령 중단 (실제 서버에서는 루프 탈출 대신 해당 기기만 건너뜁니다)
        //         break; 
        //     }
        // }
    }
    httpServer.stop();
    matterController->shutdown();
    delete matterController;
    tcpDriver->stopServer();
    delete tcpDriver;
    manager.stopHealthCheck();
    return 0;
}