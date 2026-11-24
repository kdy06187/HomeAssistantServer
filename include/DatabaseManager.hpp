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
// timestamp는 DB 저장 형식(UTC, "YYYY-MM-DD HH:MM:SS")
struct EnergyLog{
    std::string timestamp;
    int64_t total_mwh;
};

struct HistoryPage {
    std::vector<EnergyLog> logs; // 오래된 것부터
    bool truncated = false;      // 범위 안 기록이 limit보다 많아 오래된 쪽이 잘렸는지
};

struct PurgeResult {
    bool skipped = false;
    std::string reason;          // skipped일 때만. 로그 전용
    int64_t deletedLogs = 0;     // 보관 기간이 지난 이력
    int64_t deletedDevices = 0;  // 숨김 기간이 지나 완전 삭제된 기기
    int64_t deletedOrphans = 0;  // 기기 행이 없는 통계·이력 행 수
};
// findDevice 결과. 숨김이면 숨긴 시각(DB 형식)도 준다
struct DeviceRecordState {
    enum class Kind { None, Active, Hidden };
    Kind kind = Kind::None;
    std::string deletedAtUtc;  // Hidden일 때만
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
    std::vector<Device> loadActiveDevices();               // 숨기지 않은 기기만 불러오기
    bool updateDeviceState(const std::string& id, int isActive); // 상태 변경
    // 숨기지 않은 기기의 마지막 접속 IP를 바꾼다. 없는·숨긴 기기는 0행이며 실패가 아니다(DB 오류만 false)
    bool updateDeviceIp(const std::string& id, const std::string& ip);
    // 저장된 ip(없거나 NULL·DB 오류면 빈 문자열). 재등록 때 메모리를 DB와 맞추는 데 쓴다
    std::string loadDeviceIp(const std::string& id);
    // 기기를 숨긴다(통계·이력은 유지). 없거나 이미 숨긴 기기는 DeviceNotFound, DB 오류는 Internal
    Status markDeleted(const std::string& id, const std::string& deletedAtUtc);
    // 그 ID 행이 없음·활성·숨김 중 무엇인지. DB 오류는 failure(Internal)
    Result<DeviceRecordState> findDevice(const std::string& id);
    // 숨김 행 하나와 그 통계·이력을 한 트랜잭션으로 영구 삭제한다(복구 기간이 지난 기기를 새로 등록할 때).
    // 활성·없는 행은 지우지 않고 DeviceNotFound, DB 오류는 Internal(아무것도 안 지움)
    Status purgeDeviceRecords(const std::string& id);
    void close();

    bool initEnergyTable(); // 전력량 테이블 초기화
    // 실패하면 false이고 통계는 바뀌지 않는다
    bool updateEnergyStat(const std::string& deviceId, int64_t current_mwh, int64_t& out_real_total);
    bool initHistoryTable();
    bool insertEnergyLog(const std::string& deviceId, int64_t total_mwh, const std::string& timestampUtc);

    // 범위 [fromUtc, toUtc) 안의 최신 limit건을 오래된 것부터. 범위를 생략하면 그쪽은 제한 없음.
    // DB 오류·limit<=0은 빈 목록이 아니라 failure
    Result<HistoryPage> queryHistory(const std::string& deviceId, const std::optional<std::string>& fromUtc,
                                     const std::optional<std::string>& toUtc, int limit);
    // 집계용: 범위 안 전부(오래된 순) 앞에 범위 시작 직전 1건(있으면)을 붙여 돌려준다
    Result<std::vector<EnergyLog>> querySamplesForAggregation(const std::string& deviceId,
                                                              const std::string& fromUtc,
                                                              const std::string& toUtc);
    // 이력이 하나도 없으면 값 없는 성공
    Result<std::optional<std::string>> latestHistoryTime();
    // cutoffUtc보다 오래된 이력·숨김 기기·고아 행을 한 트랜잭션으로 지운다. 안전장치에 걸리면 아무것도 지우지 않는다
    Result<PurgeResult> purgeExpired(const std::string& cutoffUtc, double maxDeleteRatio = 0.5);
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
    // 정리 트랜잭션 안에서 쓰는 조각들
    Result<std::optional<std::string>> latestHistoryTimeLocked();
    std::optional<int64_t> countLocked(const char* sql, const std::string* cutoffUtc);
    std::optional<int64_t> deleteLocked(const char* sql, const std::string* cutoffUtc);
    Result<PurgeResult> purgeLocked(const std::string& cutoffUtc, double maxDeleteRatio);
    Status purgeDeviceRecordsLocked(const std::string& id);

    mutable std::mutex mutex_;
    sqlite3* db_;
};