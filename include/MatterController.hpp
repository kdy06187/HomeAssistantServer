#pragma once
#include "ProtocolDriver.hpp"
#include "IMessageSender.hpp"
#include <string>
#include <cstdint>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <lib/core/CHIPCore.h>
#include <lib/core/CHIPError.h>
#include <setup_payload/SetupPayload.h>
#include <setup_payload/ManualSetupPayloadParser.h>

class MatterController : public ProtocolDriver, public IMessageSender {
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
    // 3. 기기 제어 (제어 - Controller 역할)
    // =========================================================
    bool turnOn(uint64_t nodeId,uint16_t endpointId);
    bool turnOff(uint64_t nodeId,uint16_t endpointId);

    // 인터페이스 구현
    bool sendCommand(const std::string& deviceId, const std::string& command) override;
    Result<CommissionOutcome> commission(const CommissionRequest& request,
                                         const std::unordered_set<std::string>& activeDeviceIds) override;
    bool unpairDevice(const std::string& deviceId) override;
    std::string readDeviceState(const std::string& deviceId, bool isManualRequest) override;
    Result<EnergyReading> readEnergy(const std::string& deviceId) override;

    bool removeDeviceRegistration(const std::string& serialNumber);

    

    std::string getPowerUsage(std::string deviceId, bool isManualRequest = false);
    std::string getCumulativeEnergy(std::string deviceId, bool isManualRequest = false);

    // PC 종료 메시지는 main에서 항상 TCPDriver로 보내므로 Matter 쪽 발송은 쓰지 않는다
    Status sendTo(const std::string& /*deviceId*/, const std::string& /*jsonLine*/) override {
        return Status::failure(ErrorCode::UnsupportedOperation, "Matter 드라이버는 메시지 발송을 지원하지 않음");
    }
    void broadcast(const std::string& /*jsonLine*/) override {}
private:
    std::string mChipToolPath;
    Result<CommissionOutcome> commissionDevice(uint64_t nodeId, const std::string& name, const std::string& manualPincode,
                          const std::string& wifiSsid, const std::string& wifiPassword,
                          const std::unordered_set<std::string>& activeDeviceIds);
    bool executeCommand(const std::string& cmd);
    std::string executeCommandWithOutput(std::string cmd);
    // 셸을 거치지 않고 실행해 stdout과 stderr를 함께 반환한다. 외부 입력이 인자에 포함될 때 사용한다.
    std::string executeArgsWithOutput(const std::vector<std::string>& args);
    std::string executeCommandWithErrorResponse(const std::string& cmd, const std::string& deviceId);
    std::string onDevicePairingComplete(uint64_t nodeId, const std::string& deviceName);
    bool checkDeviceRegistered(uint64_t nodeId);
    void saveDeviceRegistration(const std::string& serialNumber,uint64_t nodeId);
    std::string mConfigFilePath;

    // 페어링 중인 기기의 NodeId를 저장
    std::unordered_set<uint64_t> mPairingNodes;

    std::unordered_map<std::string, uint64_t> mSerialToNodeId;
    std::string getDeviceSerialNumber(uint64_t nodeId);
    std::uint64_t getNodeId(const std::string& serialNumber);
    // 오프라인 상태인 기기의 NodeId(문자열)와 타임아웃 발생 시간을 기록
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> mOfflineNodes;

    bool isPairing(uint64_t nodeId);
    void setPairing(uint64_t nodeId, bool pairing);
    bool isOffline(const std::string& nodeIdText);
    void markOffline(const std::string& nodeIdText);
    bool clearOffline(const std::string& nodeIdText); // 명부에 있었으면 true

    // 두 락은 동시에 잡지 않는다. chip-tool 실행은 항상 락 밖에서 한다
    std::mutex stateMutex_; // mSerialToNodeId, mOfflineNodes, mPairingNodes
    std::mutex fileMutex_;  // mConfigFilePath 읽기·쓰기
};