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
#include "IMessageSender.hpp"
#include "Clock.hpp"
#include "DriverManager.hpp"
#include "Command.hpp"
#include <unordered_set>
#include <optional>

// E6 응답에 쓰는 값. totalEnergy_mWh는 DB 보정이 끝난 누적값
struct EnergySnapshot {
    std::string deviceState;
    int64_t activePower_mW = 0;
    int64_t totalEnergy_mWh = 0;
};

// E7 응답에 쓰는 값. logs는 오래된 것부터
struct HistoryResult {
    std::string deviceState;
    std::vector<EnergyLog> logs;
};

class DeviceManager {
public:
    DeviceManager(DatabaseManager& db, DriverManager& deviceDrivers, Clock& clock);
    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    // 시스템 초기화 API - 프로토콜 타입별로 어떤 드라이버를 쓸지 등록
    void registerDriver(ProtocolType type, ProtocolDriver* driver);

    void initFromDatabase();
    // 디바이스 등록 API - 디바이스를 등록하고, 프로토콜 타입에 맞는 드라이버를 통해 초기화.
    // DB에 먼저 저장하고, 성공했을 때만 메모리에 넣는다. 실패하면 false(메모리 불변)
    bool addDevice(std::string id, std::string name, ProtocolType protocol_type,
                   const std::vector<DriverType>& driverTypes, IMessageSender* sender);
    bool getDevice(std::string id, Device& outDevice);
    // 드라이버에 실제 상태를 묻고 바뀌었으면 저장한다. 없는 기기는 DeviceNotFound
    Result<std::string> refreshState(const std::string& id, bool isManualRefresh);
    // refreshState의 결과를 문자열로만 돌려준다(오류는 "UNKNOWN")
    std::string getDeviceState(std::string id, bool isManualRefresh = false);
    std::vector<Device> getAllDevices();
    std::string getProtocolString(ProtocolType type);
    bool updateDeviceState(std::string id, std::string newState);
    bool startCommissioning(ProtocolType type, std::string name, std::string payload, const std::string& ssid = "", const std::string& password = "",const std::vector<DriverType>& driverTypes ={});
    // 디바이스 제어 API - 디바이스 ID와 명령을 받아 해당 디바이스를 제어
    CommandResult executeCommand(std::string id, std::string command);
    bool removeDevice(std::string id);
    std::unordered_set<std::string> activeDeviceIds();

    // 모든 기기를 한 번 점검한다. 주기 호출은 PeriodicTasks가 맡는다
    void runHealthCheck();

    // 존재·드라이버 확인 → 락 밖에서 조회 → 성공할 때만 통계 반영. 실패하면 DB는 그대로다
    // (미등록 DeviceNotFound, 전력 조회 미지원 UnsupportedOperation, 드라이버가 돌려준 코드는 그대로 전달.
    //  단 드라이버의 DeviceNotFound는 서버 내부 불일치라 Internal)
    Result<EnergySnapshot> readEnergy(const std::string& deviceId);
    Result<HistoryResult> getEnergyHistory(const std::string& deviceId, int limit = 50);

private:
    std::unordered_map<ProtocolType, ProtocolDriver*> protocolDriversSnapshot();
    std::shared_ptr<std::mutex> energyLockFor(const std::string& deviceId);

    DatabaseManager& db_;
    DriverManager& deviceDrivers_;
    [[maybe_unused]] Clock& clock_;

    std::unordered_map<std::string,Device> devices_;
    std::unordered_map<ProtocolType, ProtocolDriver*> drivers_;
    // devices_·drivers_ 보호. 쥔 채로는 메모리만 다룬다(DB·드라이버·다른 락 호출 금지)
    std::mutex mutex_;

    // 기기별 전력 락(자원 락): 같은 기기의 "조회 → 존재 재확인 → 통계 저장"을 직렬화한다.
    // 두 조회가 역순으로 끝나면 작은 값이 나중에 저장돼 "카운터 초기화"로 오인되기 때문이다.
    // 쥔 채로 드라이버·mutex_·DB를 차례로 잡을 수 있다(순서: 전력 락 → 그 밖). mutex_를 쥔 채 이 락을 잡지 않는다.
    std::mutex energyLocksMutex_; // energyLocks_ 맵만 보호(메모리만)
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> energyLocks_;
};
