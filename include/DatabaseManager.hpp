#pragma once
#include <string>
#include <vector>
#include <sqlite3.h>
#include "Device.hpp"

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

private:
    DatabaseManager() : db_(nullptr) {}
    ~DatabaseManager() { close(); }

    sqlite3* db_;
};