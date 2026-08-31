#pragma once
#include "ProtocolDriver.hpp"
#include <string>
#include <cstdint>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <lib/core/CHIPCore.h>
#include <lib/core/CHIPError.h>
#include <setup_payload/SetupPayload.h>
#include <setup_payload/ManualSetupPayloadParser.h>

class MatterController : public ProtocolDriver {
public:
    MatterController(DeviceManager& deviceManager);
    ~MatterController();

    // =========================================================
    // 1. 생명주기 (Lifecycle) 관리
    // =========================================================
    /**
     * @brief Matter 스택 및 이벤트 루프를 초기화합니다.
     * @return 성공 여부 (true/false)
     */
    bool Initialize();

    /**
     * @brief Matter 스택을 안전하게 종료하고 메모리를 해제합니다.
     */
    void shutdown();

    // =========================================================
    // 3. 기기 제어 (제어 - Controller 역할)
    // =========================================================
    bool turnOn(uint64_t nodeId,uint16_t endpointId);
    bool turnOff(uint64_t nodeId,uint16_t endpointId);

    // 인터페이스 구현
    bool sendCommand(std::string deviceId, std::string command) override;
    bool commissionDevice(std::string name, std::string payload) override;
    bool unpairDevice(std::string deviceId) override;
    std::string readDeviceState(std::string deviceId, bool isManualRequest) override;

    bool removeDeviceRegistration(uint64_t nodeId);

    std::string getPowerUsage(std::string deviceId, bool isManualRequest = false);
    std::string getCumulativeEnergy(std::string deviceId, bool isManualRequest = false);

private:
    std::string mChipToolPath;
    bool commissionDevice(uint64_t nodeId, std::string name, const std::string& manualPincode,
                          const std::string& wifiSsid, const std::string& wifiPassword);
    bool executeCommand(const std::string& cmd);
    std::string executeCommandWithOutput(std::string cmd);
    std::string executeCommandWithErrorResponse(const std::string& cmd, const std::string& deviceId);
    void onDevicePairingComplete(uint64_t nodeId, const std::string& deviceName);
    bool checkDeviceRegistered(uint64_t nodeId);
    void saveDeviceRegistration(uint64_t nodeId);
    std::string mConfigFilePath;

    // 페어링 중인 기기의 NodeId를 저장
    std::unordered_set<uint64_t> mPairingNodes;
    
    // 오프라인 상태인 기기의 NodeId(문자열)와 타임아웃 발생 시간을 기록
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> mOfflineNodes;
};