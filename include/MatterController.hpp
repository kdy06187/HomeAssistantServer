#pragma once
#include "ProtocolDriver.hpp"
#include "IMessageSender.hpp"
#include "ChipToolRunner.hpp"
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
    // chip-tool은 모두 runner로 실행한다(셸 없음). runner는 이 객체보다 오래 살아야 한다
    explicit MatterController(ChipToolRunner& runner);
    ~MatterController();

    // chip-tool 제한 시간(9번 실기에서 확인)
    static constexpr std::chrono::milliseconds kCommandTimeout{5000};     // 제어·상태·전력
    static constexpr std::chrono::milliseconds kSlowCommandTimeout{10000}; // 시리얼 읽기·unpair
    static constexpr std::chrono::milliseconds kPairingTimeout{180000};   // BLE+Wi-Fi 페어링

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
    // chip-tool 하위 명령을 실행해 출력(stdout+stderr)을 돌려준다. 실행하지 못했으면 빈 문자열
    std::string runChipTool(const std::vector<std::string>& subcommand, std::chrono::milliseconds timeout,
                            const std::vector<std::string>& secretsToMask = {});
    // 제어·조회용: 성공 표지가 없거나 오류 키워드가 있으면 "OFFLINE"(오프라인 명부 갱신), 아니면 출력
    std::string runChipToolCommand(const std::vector<std::string>& subcommand, const std::string& deviceId);
    ChipToolRunner& mRunner;
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