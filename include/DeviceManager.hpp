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
    // 디바이스 등록 API - 디바이스를 등록하고, 프로토콜 타입에 맞는 드라이버를 통해 초기화
    void addDevice(std::string id, std::string name, ProtocolType protocol_type,
                   const std::vector<DriverType>& driverTypes, IMessageSender* sender);
    bool getDevice(std::string id, Device& outDevice);
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
    // (미등록 DeviceNotFound, 전력 조회 미지원 UnsupportedOperation, 조회·저장 실패 그 밖의 코드)
    Result<EnergySnapshot> readEnergy(const std::string& deviceId);
    Result<HistoryResult> getEnergyHistory(const std::string& deviceId, int limit = 50);

private:
    std::unordered_map<ProtocolType, ProtocolDriver*> protocolDriversSnapshot();

    DatabaseManager& db_;
    DriverManager& deviceDrivers_;
    [[maybe_unused]] Clock& clock_;

    std::unordered_map<std::string,Device> devices_;
    std::unordered_map<ProtocolType, ProtocolDriver*> drivers_;
    // devices_·drivers_ 보호. 쥔 채로는 메모리만 다룬다(DB·드라이버·다른 락 호출 금지)
    std::mutex mutex_;
};
