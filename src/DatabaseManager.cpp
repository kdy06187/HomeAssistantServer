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
    initEnergyTable(); // 전력량 테이블 초기화
    initHistoryTable(); // 전력량 로그 테이블 초기화
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
bool DatabaseManager::removeDevice(const std::string& device_id) {
    const char* sql = "DELETE FROM devices WHERE device_id = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_STATIC);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

bool DatabaseManager::initEnergyTable() {
    std::string sql = "CREATE TABLE IF NOT EXISTS device_energy_stats("
                      "device_id TEXT PRIMARY KEY, "
                      "real_total_mwh BIGINT DEFAULT 0, "
                      "last_plug_mwh BIGINT DEFAULT 0);";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "전력량 테이블 생성 에러: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}
bool DatabaseManager::updateEnergyStat(const std::string& deviceId, long current_mwh, long& out_real_total) {
    long db_real_total = 0;
    long db_last_plug = 0;
    bool exists = false;

    // 기존 데이터 조회 (SELECT)
    std::string selectSql = "SELECT real_total_mwh, last_plug_mwh FROM device_energy_stats WHERE device_id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, selectSql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            db_real_total = sqlite3_column_int64(stmt, 0);
            db_last_plug = sqlite3_column_int64(stmt, 1);
            exists = true;
        }
        sqlite3_finalize(stmt);
    }

    // 2. 보정 알고리즘 (Delta 계산)
    if (exists) {
        if (current_mwh < db_last_plug) {
            // 기기 초기화 발생: 새 측정값을 그대로 누적
            db_real_total += current_mwh; 
        } else {
            // 정상 동작: 이전 측정값과의 차이(Delta)만 누적
            db_real_total += (current_mwh - db_last_plug);
        }
    } else {
        // 최초 등록
        db_real_total = current_mwh;
    }

    // 갱신된 데이터 저장 (INSERT OR REPLACE)
    std::string upsertSql = "INSERT OR REPLACE INTO device_energy_stats (device_id, real_total_mwh, last_plug_mwh) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2(db_, upsertSql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, db_real_total);
        sqlite3_bind_int64(stmt, 3, current_mwh);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    out_real_total = db_real_total; // 안드로이드로 보낼 최종 누적량
    return true;
}
bool DatabaseManager::initHistoryTable() {
    std::string sql = "CREATE TABLE IF NOT EXISTS state_history_logs("
                      "log_id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "device_id TEXT, "
                      "total_mwh BIGINT, "
                      "timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP);";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "[DatabaseManager] ❌ 시계열 로그 테이블 생성 에러: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    std::cout << "[DatabaseManager] ✅ 시계열 로그 테이블(state_history_logs) 준비 완료!" << std::endl;
    return true;
}
bool DatabaseManager::insertEnergyLog(const std::string& deviceId, long total_mwh) {
    std::string sql = "INSERT INTO state_history_logs (device_id, total_mwh) VALUES (?, ?);";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, total_mwh);
        
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "[DatabaseManager] ❌ 시계열 데이터 저장 실패" << std::endl;
        }
        sqlite3_finalize(stmt);
        return true;
    }
    return false;
}
std::vector<EnergyLog> DatabaseManager::getEnergyHistory(const std::string& deviceId, int limit) {
    std::vector<EnergyLog> logs;
    
    std::string sql = "SELECT timestamp, total_mwh FROM state_history_logs "
                      "WHERE device_id = ? ORDER BY timestamp DESC LIMIT ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            EnergyLog log;
            const char* ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            log.timestamp = ts ? ts : ""; // null 방어
            log.total_mwh = sqlite3_column_int64(stmt, 1);
            logs.push_back(log);
        }
        sqlite3_finalize(stmt);
    } else {
        std::cerr << "[DatabaseManager] ❌ 시계열 데이터 조회 실패: " << sqlite3_errmsg(db_) << std::endl;
    }

    std::reverse(logs.begin(), logs.end());

    if (logs.empty()) {
        std::cout << "[DatabaseManager] 기기(" << deviceId << ")의 저장된 시계열 데이터가 없습니다." << std::endl;
    }
    return logs;
}