#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <sqlite3.h>
#include "Device.hpp"
#include <algorithm>
#include "DriverType.hpp"
#include "Result.hpp"
struct EnergyLog{
    std::string timestamp;
    int64_t total_mwh;
};
class DatabaseManager {
public:
    DatabaseManager() : db_(nullptr) {}
    ~DatabaseManager() { close(); }

    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    // backupTimestampUtc: 백업 파일명에 쓸 UTC 시각(YYYYMMDDTHHMMSSZ). 비우면 현재 시각
    bool init(const std::string& dbPath, const std::string& backupTimestampUtc = "");
    
    bool insertDevice(const Device& device);               // 기기 저장(이미 있으면 갱신, ip는 보존)
    std::vector<Device> loadAllDevices();                  // 전체 기기 불러오기
    bool updateDeviceState(const std::string& id, int isActive); // 상태 변경
    bool removeDevice(const std::string& id);             // 기기 삭제
    void close();

    bool initEnergyTable(); // 전력량 테이블 초기화
    // 실패하면 false이고 통계는 바뀌지 않는다
    bool updateEnergyStat(const std::string& deviceId, int64_t current_mwh, int64_t& out_real_total);
    bool initHistoryTable();
    bool insertEnergyLog(const std::string& deviceId, int64_t total_mwh);

    // 최신 limit건을 오래된 것부터. DB 오류·limit<=0은 빈 목록이 아니라 failure
    Result<std::vector<EnergyLog>> getEnergyHistory(const std::string& deviceId, int limit = 50);
private:
    // 아래 *Locked 함수는 호출자가 mutex_를 이미 잡았다고 가정한다
    void closeLocked();
    bool execLocked(const char* sql, const char* failLogPrefix);
    bool initEnergyTableLocked();
    bool initHistoryTableLocked();
    bool migrateToV1Locked();
    bool backupLocked(const std::string& dbPath, int currentVersion, const std::string& timestampUtc);
    // 조회 자체가 실패하면 nullopt("없음"·0과 구분한다)
    std::optional<bool> tableExistsLocked(const char* tableName);
    std::optional<int> readUserVersionLocked();

    mutable std::mutex mutex_;
    sqlite3* db_;
};