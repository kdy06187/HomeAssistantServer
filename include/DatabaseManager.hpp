#pragma once
#include <string>
#include <vector>
#include <sqlite3.h>
#include "Device.hpp"
#include <algorithm>
struct EnergyLog{
    std::string timestamp;
    long total_mwh;
};
class DatabaseManager {
public:
    static DatabaseManager& getInstance() {
        static DatabaseManager instance;
        return instance;
    }
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    bool init(const std::string& dbPath);
    
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
    DatabaseManager() : db_(nullptr) {}
    ~DatabaseManager() { close(); }

    sqlite3* db_;
};