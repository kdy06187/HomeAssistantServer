#pragma once
#include <string>
#include <iostream>
#include <unordered_map>
#include <mutex>
#include <vector>
#include "Device.hpp"
#include "ProtocolDriver.hpp"
#include "DatabaseManager.hpp"
#include "IDeviceDriver.hpp"
#include <memory>
#include "DriverType.hpp"
#include "Clock.hpp"
#include "DelayedCommandScheduler.hpp"
#include <atomic>
#include "DriverManager.hpp"
#include "Command.hpp"
#include <unordered_set>
#include <optional>
#include "EnergyAggregation.hpp"
#include "RequestValidation.hpp"

// E6 응답에 쓰는 값. totalEnergy_mWh는 DB 보정이 끝난 누적값
struct EnergySnapshot {
    std::string deviceState;
    int64_t activePower_mW = 0;
    int64_t totalEnergy_mWh = 0;
};

// E7 응답에 쓰는 값. period가 없으면 원본(logs, truncated), 있으면 집계(buckets)
struct HistoryResult {
    std::string deviceState;
    std::vector<EnergyLog> logs; // 오래된 것부터
    bool truncated = false;
    std::optional<HistoryPeriod> period;
    std::vector<EnergyBucket> buckets;
};

// E4 응답에 쓰는 값. deletedAtUtc는 DB 시각 형식
struct DeleteOutcome {
    std::string deletedAtUtc;
    bool unpaired = false; // 기기 쪽 연결 해제 성공 여부. false여도 숨김은 끝났다
};

// registerCommissioned 결과. restored는 복구 기간 안에 숨긴 같은 기기를 되살려 기록을 이어받았는지
struct RegisterOutcome {
    std::string deviceId;
    bool restored = false;
};

class DeviceManager {
public:
    // 이력 보관 기간이자 숨김 기기의 복구 가능 기간
    static constexpr int kRetentionDays = 90;

    // 프로토콜 드라이버·스케줄러는 main이 소유한다. 이들을 쓰는 스레드(HTTP·스케줄러·주기 작업)를
    // 모두 멈춘 뒤에 DeviceManager와 드라이버를 소멸시켜야 락 밖에서 쓰는 ProtocolDriver*가 안전하다.
    DeviceManager(DatabaseManager& db, DriverManager& deviceDrivers,
                  DelayedCommandScheduler& scheduler, Clock& clock);
    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    // 시스템 초기화 API - 프로토콜 타입별로 어떤 드라이버를 쓸지 등록
    void registerDriver(ProtocolType type, ProtocolDriver* driver);

    void initFromDatabase();
    // 디바이스 등록 API - 디바이스를 등록하고, 프로토콜 타입에 맞는 드라이버를 통해 초기화.
    // DB에 먼저 저장하고, 성공했을 때만 메모리에 넣는다. 실패하면 false(메모리 불변)
    bool addDevice(std::string id, std::string name, ProtocolType protocol_type,
                   const std::vector<DriverType>& driverTypes, const std::string& ip = "");
    bool getDevice(std::string id, Device& outDevice);
    // 드라이버에 실제 상태를 묻고 바뀌었으면 저장한다. 없는 기기는 DeviceNotFound
    Result<std::string> refreshState(const std::string& id, bool isManualRefresh);
    // refreshState의 결과를 문자열로만 돌려준다(오류는 "UNKNOWN")
    std::string getDeviceState(std::string id, bool isManualRefresh = false);
    std::vector<Device> getAllDevices();
    std::string getProtocolString(ProtocolType type);
    bool updateDeviceState(std::string id, std::string newState);
    // 커미셔닝 결과를 서버에 등록하고 드라이버 확정 훅(finalizeCommission)까지 부른다. 같은 ID의 removeDevice와 차례로 실행된다.
    // DB: 활성이면 AlreadyRegistered(덮어쓰지 않음), 복구 기간 안 숨김이면 복구(restored), 지났으면 기록 삭제 뒤 새 기기, 없으면 새 기기.
    // 실패: 빈 ID CommissionFailed, 종료 중 Cancelled, DB 오류 Internal — 모두 드라이버에 accepted=false를 알린다
    Result<RegisterOutcome> registerCommissioned(ProtocolType type, const CommissionOutcome& outcome,
                                                 const std::string& name, const std::vector<DriverType>& driverTypes);
    // 숨긴 시각부터 복구 기간(kRetentionDays) 안이면 true. 시각을 읽지 못하면 기록을 지우지 않도록 true
    static bool isWithinRestorePeriod(const std::string& deletedAtUtc, std::chrono::system_clock::time_point now);
    // 디바이스 제어 API - 디바이스 ID와 명령을 받아 해당 디바이스를 제어
    CommandResult executeCommand(std::string id, std::string command);
    // 숨김 삭제. 없는(이미 숨긴) 기기는 DeviceNotFound, 숨김 저장 실패는 Internal(기기는 그대로 남는다)
    Result<DeleteOutcome> removeDevice(const std::string& id);
    std::unordered_set<std::string> activeDeviceIds();

    // TCP 기기 연결이 맵에 들어갔을 때(TcpServer 수락 스레드, 락 없이) 부른다.
    // 메모리의 ip가 다르면 바꾸고 DB에도 쓴다. 아직 메모리에 없는 ID는 DB 갱신만 시도한다(숨긴·없는 기기는 0행)
    void onTcpConnected(const std::string& deviceId, const std::string& ip);

    // 종료가 시작됐음을 알린다. 이후 드라이버 호출 실패(취소된 chip-tool 등)는 기기 상태·DB에 기록하지 않는다
    void beginShutdown() { isShuttingDown_ = true; }

    // 모든 기기를 한 번 점검한다. 주기 호출은 PeriodicTasks가 맡는다
    void runHealthCheck();

    // 존재·드라이버 확인 → 락 밖에서 조회 → 성공할 때만 통계 반영. 실패하면 DB는 그대로다
    // (미등록 DeviceNotFound, 전력 조회 미지원 UnsupportedOperation, 드라이버가 돌려준 코드는 그대로 전달.
    //  단 드라이버의 DeviceNotFound는 서버 내부 불일치라 Internal)
    Result<EnergySnapshot> readEnergy(const std::string& deviceId);
    // 없는·숨긴 기기 DeviceNotFound, 전력 미지원 프로토콜 UnsupportedOperation(readEnergy와 같은 판정)
    Result<HistoryResult> getEnergyHistory(const std::string& deviceId, const HistoryQuery& query);

    // 보관 기간이 지난 이력·숨김 기기를 지운다. 주기 호출은 PeriodicTasks가 맡는다
    Result<PurgeResult> runRetentionCleanup();

private:
    bool isRegistered(const std::string& id);
    // 드라이버에 명령을 보내고 결과에 따라 상태를 갱신한다. 즉시 실행과 지연 만기 실행이 함께 쓴다
    CommandResult executeHardware(const std::string& id, const std::string& command,
                                  ProtocolDriver* protocolDriver, const std::string& currentState);
    static bool supportsEnergy(ProtocolType type);
    // 락 밖에서 부른다. 실패·예외는 false
    bool unpairQuietly(ProtocolDriver* protocolDriver, const std::string& id);
    Result<HistoryResult> aggregateHistory(const std::string& deviceId, const HistoryQuery& query, HistoryResult history);
    std::shared_ptr<std::mutex> energyLockFor(const std::string& deviceId);
    std::shared_ptr<std::mutex> registrationLockFor(const std::string& deviceId);
    // 예외는 로그만 남긴다(등록 결과를 바꾸지 않는다)
    void finalizeQuietly(ProtocolDriver* driver, const CommissionOutcome& outcome, bool accepted);

    DatabaseManager& db_;
    DriverManager& deviceDrivers_;
    DelayedCommandScheduler& scheduler_;
    Clock& clock_;
    std::atomic<bool> isShuttingDown_{false};

    std::unordered_map<std::string,Device> devices_;
    std::unordered_map<ProtocolType, ProtocolDriver*> drivers_;
    // devices_·drivers_ 보호. 쥔 채로는 메모리만 다룬다(DB·드라이버·다른 락 호출 금지)
    std::mutex mutex_;

    // 기기별 전력 락(자원 락): 같은 기기의 "조회 → 존재 재확인 → 통계 저장"을 직렬화한다.
    // 두 조회가 역순으로 끝나면 작은 값이 나중에 저장돼 "카운터 초기화"로 오인되기 때문이다.
    // 쥔 채로 드라이버·mutex_·DB를 차례로 잡을 수 있다(순서: 전력 락 → 그 밖). mutex_를 쥔 채 이 락을 잡지 않는다.
    std::mutex energyLocksMutex_; // energyLocks_ 맵만 보호(메모리만)
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> energyLocks_;

    // 기기별 등록 락(자원 락): 같은 ID의 removeDevice 전체(맵 제거 → 숨김 → 예약 취소 → unpair)와
    // registerCommissioned 전체(DB 판정 → 저장 → 메모리 → 확정 훅)를 차례로 실행한다. 늦게 끝난 unpair가
    // 방금 등록한 새 노드를 지우는 일을 막는다. 가장 바깥 락이다: 쥔 채 mutex_·DB·드라이버를 차례로 잡을 수 있고,
    // mutex_나 전력 락을 쥔 채 이 락을 잡지 않는다
    std::mutex registrationLocksMutex_;  // registrationLocks_ 맵만 보호(메모리만)
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> registrationLocks_;
};
