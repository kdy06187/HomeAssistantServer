#include "DatabaseManager.hpp"
#include <iostream>
#include "DriverType.hpp"
#include <sstream>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {

constexpr int kSchemaVersion = 1;
constexpr size_t kBackupsToKeep = 3;

std::string makeUtcTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
    return buf;
}

// 파일명 끝의 시각 부분(마지막 '-' 뒤)으로 정렬해야 버전 자릿수(v2, v10)와 무관하게 시간순이 된다
std::string backupTimeKey(const std::string& fileName) {
    return fileName.substr(fileName.rfind('-') + 1);
}

// 방금 만든 백업(keepName)은 시계가 뒤로 간 경우에도 지우지 않는다
void pruneOldBackups(const std::filesystem::path& dbPath, const std::string& keepName) {
    namespace fs = std::filesystem;
    const std::string prefix = dbPath.filename().string() + ".bak-v";
    fs::path dir = dbPath.parent_path().empty() ? fs::path(".") : dbPath.parent_path();

    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::string name = entry.path().filename().string();
        if (name != keepName && name.compare(0, prefix.size(), prefix) == 0) names.push_back(name);
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return std::make_pair(backupTimeKey(a), a) < std::make_pair(backupTimeKey(b), b);
    });
    for (size_t i = 0; i + (kBackupsToKeep - 1) < names.size(); ++i) {
        fs::remove(dir / names[i], ec);
    }
}

bool backupPassesIntegrityCheck(const std::string& backupPath) {
    sqlite3* conn = nullptr;
    if (sqlite3_open_v2(backupPath.c_str(), &conn, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close_v2(conn);
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(conn, "PRAGMA integrity_check;", -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        ok = text && std::string(reinterpret_cast<const char*>(text)) == "ok";
    }
    sqlite3_finalize(stmt);
    sqlite3_close_v2(conn);
    return ok;
}

}  // namespace

bool DatabaseManager::init(const std::string& dbPath, const std::string& backupTimestampUtc) {
    std::lock_guard<std::mutex> lock(mutex_);

    const bool isMemory = (dbPath == ":memory:");
    // CREATE 전에 판단해야 "원래 있던 DB"인지 알 수 있다
    const bool fileExisted = !isMemory && std::filesystem::exists(dbPath);

    closeLocked();  // 다시 init하면 이전 연결을 닫는다
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc) {
        std::cerr << "[DB] 연결 실패: " << sqlite3_errmsg(db_) << std::endl;
        closeLocked();
        return false;
    }

    // 순서: 버전 확인 → (필요하면) 백업 → 테이블 생성 → 마이그레이션.
    // 백업을 먼저 해야 실패했을 때 원본에 아무것도 쓰지 않은 상태로 멈출 수 있다
    const auto currentVersion = readUserVersionLocked();
    if (!currentVersion) {
        std::cerr << "[DB] 스키마 버전 확인 실패: " << sqlite3_errmsg(db_) << std::endl;
        closeLocked();
        return false;
    }
    bool hadDevicesTable = false;
    if (fileExisted) {
        const auto exists = tableExistsLocked("devices");
        if (!exists) {
            std::cerr << "[DB] 테이블 확인 실패: " << sqlite3_errmsg(db_) << std::endl;
            closeLocked();
            return false;
        }
        hadDevicesTable = *exists;
    }
    if (*currentVersion < kSchemaVersion && hadDevicesTable) {
        std::string stamp = backupTimestampUtc.empty() ? makeUtcTimestamp() : backupTimestampUtc;
        if (!backupLocked(dbPath, *currentVersion, stamp)) {
            std::cerr << "[DB] 백업 실패, 마이그레이션을 중단합니다" << std::endl;
            closeLocked();
            return false;
        }
    }

    if (!execLocked("CREATE TABLE IF NOT EXISTS devices("
                    "device_id TEXT PRIMARY KEY, "
                    "display_name TEXT, "
                    "protocol_type INTEGER, "
                    "is_active INTEGER, "
                    "driverTypes TEXT);", "[DB] 테이블 생성 에러: ") ||
        !initEnergyTableLocked() ||   // 전력량 테이블
        !initHistoryTableLocked()) {  // 전력량 로그 테이블
        closeLocked();
        return false;
    }

    if (*currentVersion < kSchemaVersion && !migrateToV1Locked()) {
        closeLocked();
        return false;
    }
    std::cout << "[DB] SQLite 로드 성공 (" << dbPath << ")" << std::endl;
    return true;
}

void DatabaseManager::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
}

void DatabaseManager::closeLocked() {
    // close_v2: 정리 안 된 문장이 남아도 연결을 "좀비"로 두었다가 마지막 finalize 때 닫는다(close는 BUSY로 실패)
    if (db_) { sqlite3_close_v2(db_); db_ = nullptr; }
}

bool DatabaseManager::execLocked(const char* sql, const char* failLogPrefix) {
    if (!db_) {
        std::cerr << failLogPrefix << "DB가 열려 있지 않음" << std::endl;
        return false;
    }
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << failLogPrefix << (errMsg ? errMsg : "") << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

std::optional<bool> DatabaseManager::tableExistsLocked(const char* tableName) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, tableName, -1, SQLITE_STATIC);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    return std::nullopt;
}

std::optional<int> DatabaseManager::readUserVersionLocked() {
    sqlite3_stmt* stmt = nullptr;
    std::optional<int> version;
    if (sqlite3_prepare_v2(db_, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

bool DatabaseManager::backupLocked(const std::string& dbPath, int currentVersion,
                                   const std::string& timestampUtc) {
    const std::string backupPath = dbPath + ".bak-v" + std::to_string(currentVersion) + "-" + timestampUtc;

    // O_EXCL: 기존 파일을 덮어쓰지 않고, 처음부터 0600으로 만들어 권한 노출 구간을 없앤다
    int fd = ::open(backupPath.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) {
        std::cerr << "[DB] 백업 파일 생성 실패: " << std::strerror(errno) << std::endl;
        return false;
    }
    ::close(fd);

    auto failAndRemove = [&](const char* reason) {
        std::cerr << "[DB] 백업 실패: " << reason << std::endl;
        std::error_code ec;
        std::filesystem::remove(backupPath, ec);
        return false;
    };

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "VACUUM INTO ?;", -1, &stmt, nullptr) != SQLITE_OK) {
        std::string reason = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return failAndRemove(reason.c_str());
    }
    sqlite3_bind_text(stmt, 1, backupPath.c_str(), -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return failAndRemove("VACUUM INTO");

    if (!backupPassesIntegrityCheck(backupPath)) return failAndRemove("무결성 검사");
    if (::chmod(backupPath.c_str(), 0600) != 0) return failAndRemove("권한 설정");

    pruneOldBackups(dbPath, std::filesystem::path(backupPath).filename().string());
    std::cout << "[DB] 백업 완료: " << backupPath << std::endl;
    return true;
}

bool DatabaseManager::migrateToV1Locked() {
    const char* sql =
        "BEGIN IMMEDIATE;"
        "ALTER TABLE devices ADD COLUMN ip TEXT;"
        "ALTER TABLE devices ADD COLUMN deleted_at TEXT;"
        "CREATE INDEX IF NOT EXISTS idx_history_device_time ON state_history_logs(device_id, timestamp);"
        "UPDATE devices SET ip = substr(device_id, 5) WHERE protocol_type = 1 AND device_id LIKE 'ARD_%';"
        "PRAGMA user_version = 1;"
        "COMMIT;";
    char* errMsg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        // 에러 메시지에 스키마 정보가 섞일 수 있어 로그에만 남긴다
        std::cerr << "[DB] 마이그레이션 실패: " << (errMsg ? errMsg : "") << std::endl;
        sqlite3_free(errMsg);
        // BEGIN 이후 실패면 트랜잭션이 열린 채이므로 되돌린다(열리지 않았다면 에러는 무시)
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

// 기기 저장하기 (INSERT)
bool DatabaseManager::insertDevice(const Device& device) {
    // INSERT OR REPLACE는 행을 지우고 다시 넣어 ip 등 다른 열을 NULL로 되돌리므로 UPSERT로 갱신한다.
    // 다시 등록된 기기는 활성이므로 deleted_at은 지운다.
    const char* sql = "INSERT INTO devices (device_id, display_name, protocol_type, is_active, driverTypes) VALUES (?, ?, ?, ?, ?) "
                      "ON CONFLICT(device_id) DO UPDATE SET display_name = excluded.display_name, "
                      "protocol_type = excluded.protocol_type, is_active = excluded.is_active, "
                      "driverTypes = excluded.driverTypes, deleted_at = NULL;";
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    sqlite3_stmt* stmt;
    std::cout << "[DB] 기기 저장: " << device.id << std::endl;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[DB 에러] 기기 저장 준비 실패: " << sqlite3_errmsg(db_) << "\n";
        return false;
    }

    // Device 객체의 데이터를 SQL 문장에 바인딩
    sqlite3_bind_text(stmt, 1, device.id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, device.name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, static_cast<int>(device.protocol_type));
    
    // "ON"이면 1, "OFF"면 0으로 변환해서 저장
    int isActive = (device.state == "ON" || device.state == "TURN_ON") ? 1 : 0;
    sqlite3_bind_int(stmt, 4, isActive);

    std::string driverStr = "";
    for (size_t i = 0; i < device.driver_types.size(); ++i) {
        driverStr += std::to_string(static_cast<int>(device.driver_types[i]));
        if (i != device.driver_types.size() - 1) {
            driverStr += ","; // 마지막 요소가 아니면 쉼표 추가
        }
    }
    sqlite3_bind_text(stmt, 5, driverStr.c_str(), -1, SQLITE_TRANSIENT);
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!success) {
        std::cerr << "[DB 에러] 기기 저장 실패: " << sqlite3_errmsg(db_) << "\n";
    }
    sqlite3_finalize(stmt);
    
    return success;
}

// 부팅 시 DB에서 전체 기기 읽어오기 (SELECT)
std::vector<Device> DatabaseManager::loadAllDevices() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Device> devices;
    if (!db_) return devices;
    const char* sql = "SELECT device_id, display_name, protocol_type, is_active, driverTypes FROM devices;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[DB 로드 에러] 쿼리 준비 실패: " << sqlite3_errmsg(db_) << '\n';
        return devices;
    }

    int stepResult = SQLITE_DONE;
    while ((stepResult = sqlite3_step(stmt)) == SQLITE_ROW) {
        // NULL 열을 std::string에 그대로 넣으면 널 포인터 역참조가 된다
        const unsigned char* idText = sqlite3_column_text(stmt, 0);
        if (idText == nullptr) {
            std::cerr << "[DB] device_id가 NULL인 행을 건너뜁니다" << std::endl;
            continue;
        }
        const unsigned char* nameText = sqlite3_column_text(stmt, 1);
        Device dev;
        dev.id = reinterpret_cast<const char*>(idText);
        dev.name = nameText ? reinterpret_cast<const char*>(nameText) : "";
        dev.protocol_type = static_cast<ProtocolType>(sqlite3_column_int(stmt, 2));
        
        int isActive = sqlite3_column_int(stmt, 3);
        dev.state = (isActive == 1) ? "ON" : "OFF"; // 1/0을 다시 문자열로 복원
        
        const unsigned char* driverTypesText = sqlite3_column_text(stmt, 4);
        if (driverTypesText != nullptr) {
            std::string driverStr = reinterpret_cast<const char*>(driverTypesText);
            
            // 3. 쉼표(,)를 기준으로 문자열 분리 및 변환
            std::stringstream ss(driverStr);
            std::string token;
            
            while (std::getline(ss, token, ',')) {
                if (!token.empty()) {
                    try {
                        int typeNum = std::stoi(token);
                        dev.driver_types.push_back(static_cast<DriverType>(typeNum));
                    } catch (const std::exception& e) {
                        // 잘못된 문자열이 저장되어 있을 경우의 예외 처리
                        std::cerr << "[DB] 드라이버 타입 파싱 에러: " << e.what() << '\n';
                    }
                }
            }
        }

        devices.push_back(dev);
    }
    if (stepResult != SQLITE_DONE) {
        // 읽던 도중 오류: 지금까지 읽은 기기만 돌려준다(부팅을 막지 않는다)
        std::cerr << "[DB 로드 에러] 기기 읽기 중단: " << sqlite3_errmsg(db_) << '\n';
    }
    sqlite3_finalize(stmt);
    return devices;
}

// 기기 켜고 끌 때 DB 상태 업데이트 (UPDATE)
bool DatabaseManager::updateDeviceState(const std::string& device_id, int is_active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "UPDATE devices SET is_active = ? WHERE device_id = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[DB 에러] 상태 저장 준비 실패(" << device_id << "): " << sqlite3_errmsg(db_) << "\n";
        return false;
    }

    sqlite3_bind_int(stmt, 1, is_active);
    sqlite3_bind_text(stmt, 2, device_id.c_str(), -1, SQLITE_STATIC);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!success) std::cerr << "[DB 에러] 상태 저장 실패(" << device_id << "): " << sqlite3_errmsg(db_) << "\n";
    sqlite3_finalize(stmt);
    return success;
}
bool DatabaseManager::removeDevice(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "DELETE FROM devices WHERE device_id = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[DB 에러] 기기 삭제 준비 실패(" << device_id << "): " << sqlite3_errmsg(db_) << "\n";
        return false;
    }

    sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_STATIC);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!success) std::cerr << "[DB 에러] 기기 삭제 실패(" << device_id << "): " << sqlite3_errmsg(db_) << "\n";
    sqlite3_finalize(stmt);
    return success;
}

bool DatabaseManager::initEnergyTable() {
    std::lock_guard<std::mutex> lock(mutex_);
    return initEnergyTableLocked();
}

bool DatabaseManager::initEnergyTableLocked() {
    return execLocked("CREATE TABLE IF NOT EXISTS device_energy_stats("
                      "device_id TEXT PRIMARY KEY, "
                      "real_total_mwh BIGINT DEFAULT 0, "
                      "last_plug_mwh BIGINT DEFAULT 0);", "전력량 테이블 생성 에러: ");
}

bool DatabaseManager::updateEnergyStat(const std::string& deviceId, int64_t current_mwh, int64_t& out_real_total) {
    if (current_mwh < 0) {
        // 음수 누적값은 기기 오류다. 받아들이면 "카운터 초기화"로 오인해 합계가 줄어든다
        std::cerr << "[DB] 음수 누적 전력값 거부: " << deviceId << std::endl;
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // 조회→계산→저장을 한 트랜잭션으로 묶어 중간 상태가 보이지 않게 한다
    if (!execLocked("BEGIN IMMEDIATE;", "[DB] 전력량 트랜잭션 시작 실패: ")) return false;
    auto rollback = [this](const char* reason) {
        std::cerr << "[DB] 전력량 저장 실패(" << reason << "), 롤백합니다: " << sqlite3_errmsg(db_) << std::endl;
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    };

    int64_t db_real_total = 0;
    int64_t db_last_plug = 0;
    bool exists = false;

    const char* selectSql = "SELECT real_total_mwh, last_plug_mwh FROM device_energy_stats WHERE device_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, selectSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback("조회 준비");
    sqlite3_bind_text(stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
    // 행 없음(DONE)만 "최초 등록"이다. 그 밖의 결과를 최초로 보면 누적값을 잃는다
    const int stepResult = sqlite3_step(stmt);
    if (stepResult == SQLITE_ROW) {
        db_real_total = sqlite3_column_int64(stmt, 0);
        db_last_plug = sqlite3_column_int64(stmt, 1);
        exists = true;
    }
    sqlite3_finalize(stmt);
    if (stepResult != SQLITE_ROW && stepResult != SQLITE_DONE) return rollback("조회");

    // 보정 알고리즘 (Delta 계산)
    if (exists) {
        if (current_mwh < db_last_plug) {
            // 기기 초기화 발생: 새 측정값을 그대로 누적
            db_real_total += current_mwh;
        } else {
            // 정상 동작: 이전 측정값과의 차이(Delta)만 누적
            db_real_total += (current_mwh - db_last_plug);
        }
    } else {
        db_real_total = current_mwh; // 최초 등록
    }

    const char* upsertSql = "INSERT OR REPLACE INTO device_energy_stats (device_id, real_total_mwh, last_plug_mwh) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2(db_, upsertSql, -1, &stmt, nullptr) != SQLITE_OK) return rollback("저장 준비");
    sqlite3_bind_text(stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, db_real_total);
    sqlite3_bind_int64(stmt, 3, current_mwh);
    const bool saved = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    if (!saved) return rollback("저장");
    if (!execLocked("COMMIT;", "[DB] 전력량 커밋 실패: ")) return rollback("커밋");

    out_real_total = db_real_total; // 안드로이드로 보낼 최종 누적량
    return true;
}
bool DatabaseManager::initHistoryTable() {
    std::lock_guard<std::mutex> lock(mutex_);
    return initHistoryTableLocked();
}

bool DatabaseManager::initHistoryTableLocked() {
    bool ok = execLocked("CREATE TABLE IF NOT EXISTS state_history_logs("
                         "log_id INTEGER PRIMARY KEY AUTOINCREMENT, "
                         "device_id TEXT, "
                         "total_mwh BIGINT, "
                         "timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP);",
                         "[DatabaseManager] ❌ 시계열 로그 테이블 생성 에러: ");
    if (ok) std::cout << "[DatabaseManager] ✅ 시계열 로그 테이블(state_history_logs) 준비 완료!" << std::endl;
    return ok;
}
bool DatabaseManager::insertEnergyLog(const std::string& deviceId, int64_t total_mwh) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    const char* sql = "INSERT INTO state_history_logs (device_id, total_mwh) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[DatabaseManager] ❌ 시계열 데이터 저장 준비 실패: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, total_mwh);
    const bool saved = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!saved) {
        std::cerr << "[DatabaseManager] ❌ 시계열 데이터 저장 실패: " << sqlite3_errmsg(db_) << std::endl;
    }
    sqlite3_finalize(stmt);
    return saved;
}
Result<std::vector<EnergyLog>> DatabaseManager::getEnergyHistory(const std::string& deviceId, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return Result<std::vector<EnergyLog>>::failure(ErrorCode::Internal, "DB가 열려 있지 않음");
    if (limit <= 0) {
        // SQLite는 LIMIT 음수를 "제한 없음"으로 본다. 조용히 전체를 돌려주지 않도록 거부한다
        return Result<std::vector<EnergyLog>>::failure(ErrorCode::InvalidRequest, "limit은 1 이상");
    }
    std::vector<EnergyLog> logs;

    const char* sql = "SELECT timestamp, total_mwh FROM state_history_logs "
                      "WHERE device_id = ? ORDER BY timestamp DESC, log_id DESC LIMIT ?;"; // 같은 초면 넣은 순서
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[DatabaseManager] ❌ 시계열 데이터 조회 실패: " << sqlite3_errmsg(db_) << std::endl;
        return Result<std::vector<EnergyLog>>::failure(ErrorCode::Internal, "시계열 조회 준비 실패");
    }
    sqlite3_bind_text(stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    int stepResult = SQLITE_DONE;
    while ((stepResult = sqlite3_step(stmt)) == SQLITE_ROW) {
        EnergyLog log;
        const char* ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        log.timestamp = ts ? ts : ""; // null 방어
        log.total_mwh = sqlite3_column_int64(stmt, 1);
        logs.push_back(log);
    }
    sqlite3_finalize(stmt);
    if (stepResult != SQLITE_DONE) {
        std::cerr << "[DatabaseManager] ❌ 시계열 데이터 읽기 중 오류: " << sqlite3_errmsg(db_) << std::endl;
        return Result<std::vector<EnergyLog>>::failure(ErrorCode::Internal, "시계열 읽기 실패");
    }

    std::reverse(logs.begin(), logs.end());

    if (logs.empty()) {
        std::cout << "[DatabaseManager] 기기(" << deviceId << ")의 저장된 시계열 데이터가 없습니다." << std::endl;
    }
    return Result<std::vector<EnergyLog>>::success(std::move(logs));
}