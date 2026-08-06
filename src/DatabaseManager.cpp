#include "DatabaseManager.hpp"
#include <iostream>

bool DatabaseManager::init(const std::string& dbPath) {
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc) {
        std::cerr << "[DB] 연결 실패: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    std::string sql = "CREATE TABLE IF NOT EXISTS devices("
                      "device_id TEXT PRIMARY KEY, "
                      "display_name TEXT, "
                      "protocol_type INTEGER, "
                      "is_active INTEGER);";
    char* errMsg = nullptr;
    rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] 테이블 생성 에러: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    std::cout << "[DB] SQLite 로드 성공 (" << dbPath << ")" << std::endl;
    return true;
}

void DatabaseManager::close() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

// 기기 저장하기 (INSERT)
bool DatabaseManager::insertDevice(const Device& device) {
    const char* sql = "INSERT OR REPLACE INTO devices (device_id, display_name, protocol_type, is_active) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    // Device 객체의 데이터를 SQL 문장에 바인딩
    sqlite3_bind_text(stmt, 1, device.id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, device.name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, static_cast<int>(device.protocol_type));
    
    // "ON"이면 1, "OFF"면 0으로 변환해서 저장
    int isActive = (device.state == "ON" || device.state == "TURN_ON") ? 1 : 0;
    sqlite3_bind_int(stmt, 4, isActive);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

// 부팅 시 DB에서 전체 기기 읽어오기 (SELECT)
std::vector<Device> DatabaseManager::loadAllDevices() {
    std::vector<Device> devices;
    const char* sql = "SELECT device_id, display_name, protocol_type, is_active FROM devices;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return devices;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Device dev;
        dev.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        dev.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        dev.protocol_type = static_cast<ProtocolType>(sqlite3_column_int(stmt, 2));
        
        int isActive = sqlite3_column_int(stmt, 3);
        dev.state = (isActive == 1) ? "ON" : "OFF"; // 1/0을 다시 문자열로 복원

        devices.push_back(dev);
    }
    sqlite3_finalize(stmt);
    return devices;
}

// 기기 켜고 끌 때 DB 상태 업데이트 (UPDATE)
bool DatabaseManager::updateDeviceState(const std::string& device_id, int is_active) {
    const char* sql = "UPDATE devices SET is_active = ? WHERE device_id = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, is_active);
    sqlite3_bind_text(stmt, 2, device_id.c_str(), -1, SQLITE_STATIC);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}