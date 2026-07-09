#include <iostream>
#include <thread>
#include <chrono>
#include <sqlite3.h>
#include "DeviceManager.hpp"
#include "TCPDriver.hpp"

bool initDatabase() {
    sqlite3* db;
    int rc = sqlite3_open("../db/home_assistant.db", &db);
    if (rc) {
        std::cerr << "DB 연결 실패: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    
    std::string sql = "CREATE TABLE IF NOT EXISTS devices("
                      "device_id TEXT PRIMARY KEY, "
                      "display_name TEXT, "
                      "protocol_type INTEGER, "
                      "is_active INTEGER);";
    char* errMsg = nullptr;
    rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "테이블 생성 에러: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    sqlite3_close(db);
    return true;
}

int main() {
    std::cout << "=== 🏠 홈 어시스턴트 코어 부팅 ===" << std::endl;

    // 0. 데이터베이스 준비
    if (!initDatabase()) return 1;
    std::cout << "✅ 데이터베이스 로드 완료!" << std::endl;

    // 1. 시스템 두뇌(DeviceManager) 가져오기
    DeviceManager& manager = DeviceManager::getInstance();

    // 2. TCP 드라이버 생성 및 포트 8080으로 서버 시작
    TCPDriver* tcpDriver = new TCPDriver();
    tcpDriver->startServer(8080);

    // 3. DeviceManager에 드라이버 등록
    manager.registerDriver(ProtocolType::TCP_DIY, tcpDriver);

    // 4. 관리 대상 기기 임시 등록
    manager.addDevice("Arduino_1", "거실 전등", ProtocolType::TCP_DIY);

    // 5. 서버 메인 루프 (프로그램이 꺼지지 않게 무한 대기하며 10초마다 명령 테스트)
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        std::cout << "\n[Main] 시스템 테스트: 'Arduino_1' 기기에 제어 명령 하달!" << std::endl;
        manager.executeCommand("Arduino_1", "TURN_ON");
    }

    return 0;
}