#pragma once
#include <mutex>
#include <string>
#include <vector>
#include <sqlite3.h>
#include "Device.hpp"
#include <algorithm>
#include "DriverType.hpp"
struct EnergyLog{
    std::string timestamp;
    long total_mwh;
};
class DatabaseManager {
public:
    // 테스트·주입용으로 공개한다. 싱글톤은 추후 제거 예정
    DatabaseManager() : db_(nullptr) {}
    ~DatabaseManager() { close(); }

    static DatabaseManager& getInstance() {
        static DatabaseManager instance;
        return instance;
    }
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    // backupTimestampUtc: 백업 파일명에 쓸 UTC 시각(YYYYMMDDTHHMMSSZ). 비우면 현재 시각
    bool init(const std::string& dbPath, const std::string& backupTimestampUtc = "");
    
    bool insertDevice(const Device& device);               // 기기 저장
    std::vector<Device> loadAllDevices();                  // 전체 기기 불러오기
    bool updateDeviceState(const std::string& id, int isActive); // 상태 변경
    bool removeDevice(const std::string& id);             // 기기 삭제
    void close();

    bool initEnergyTable(); // 전력량 테이블 초기화
    bool updateEnergyStat(const std::string& deviceId, long current_mwh, long& out_real_total);
    bool initHistoryTable();
    bool insertEnergyLog(const std::string& deviceId, long total_mwh);
    
    std::vector<EnergyLog> getEnergyHistory(const std::string& deviceId, int limit = 50);
private:
    // 아래 *Locked 함수는 호출자가 mutex_를 이미 잡았다고 가정한다
    void closeLocked();
    bool execLocked(const char* sql, const char* failLogPrefix);
    bool initEnergyTableLocked();
    bool initHistoryTableLocked();
    bool migrateToV1Locked();
    bool backupLocked(const std::string& dbPath, int currentVersion, const std::string& timestampUtc);
    bool tableExistsLocked(const char* tableName);
    int readUserVersionLocked();

    mutable std::mutex mutex_;
    sqlite3* db_;
};