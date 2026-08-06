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
    MatterController();
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
    // 2. 커미셔닝 (기기 등록 - Commissioner 역할)
    // =========================================================
    /**
     * @brief 새로운 Matter 기기를 서버의 네트워크에 등록합니다.
     * @param nodeId 서버가 기기에 부여할 고유 ID (예: 1, 2, 3...)
     * @param setupPinCode 기기의 11자리 핀 코드 (예: 34460414140)
     * @param discriminator 기기의 4자리 식별자
     * @return 커미셔닝 성공 여부
     */
    bool commissionDevice(uint64_t nodeId, const std::string& manualPincode,
                          const std::string& wifiSsid, const std::string& wifiPassword);

    // =========================================================
    // 3. 기기 제어 (제어 - Controller 역할)
    // =========================================================
    bool turnOn(uint64_t nodeId,uint16_t endpointId);
    bool turnOff(uint64_t nodeId,uint16_t endpointId);

    // 명령어 전송 인터페이스 구현
    bool sendCommand(std::string deviceId, std::string command) override;
    
    void removeDeviceRegistration(uint64_t nodeId);

private:
    std::string mChipToolPath;
    bool executeCommand(const std::string& cmd);
    
    bool checkDeviceRegistered(uint64_t nodeId);
    void saveDeviceRegistration(uint64_t nodeId);
    std::string mConfigFilePath;
};