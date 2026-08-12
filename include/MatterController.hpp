#pragma once
#include "ProtocolDriver.hpp"
#include <string>
#include <cstdint>
#include <thread>

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
    bool removeDeviceRegistration(uint64_t nodeId);

private:
    std::string mChipToolPath;
    bool commissionDevice(uint64_t nodeId, std::string name, const std::string& manualPincode,
                          const std::string& wifiSsid, const std::string& wifiPassword);

    bool executeCommand(const std::string& cmd);
    void onDevicePairingComplete(uint64_t nodeId, const std::string& deviceName);
    bool checkDeviceRegistered(uint64_t nodeId);
    void saveDeviceRegistration(uint64_t nodeId);
    std::string mConfigFilePath;
};