#pragma once
#include "ProtocolDriver.hpp"
#include "ChipToolRunner.hpp"
#include <string>
#include <cstdint>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include "MatterSetupCode.hpp"

class MatterController : public ProtocolDriver {
public:
    // chip-tool은 모두 runner로 실행한다(셸 없음). runner는 이 객체보다 오래 살아야 한다.
    // parseSetupPin: 설정 코드 → PIN(실제는 parseManualSetupPin, 테스트는 가짜). 경로는 build/ 기준 기본값
    // storageDirectory: chip-tool 저장소(fabric 키·KVS). 기본값 /tmp는 tmpfs라 재부팅하면 페어링이 사라지므로
    // 영구 폴더를 모든 chip-tool 실행에 --storage-directory로 넘긴다. Initialize가 0700으로 만든다
    MatterController(ChipToolRunner& runner, SetupPinParser parseSetupPin,
                     std::string chipToolPath = "../third_party/connectedhomeip/out/host/chip-tool",
                     std::string configFilePath = "../config/registered_devices.txt",
                     std::string storageDirectory = "../config/chip-tool");
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
    // 그 노드가 페어링 중인지(상태 확인·테스트용, 메모리만 읽는다)
    bool isPairing(uint64_t nodeId);

    

    std::string getPowerUsage(std::string deviceId, bool isManualRequest = false);
    std::string getCumulativeEnergy(std::string deviceId, bool isManualRequest = false);

private:
    // 노드 번호를 받는 전력 조회. readEnergy가 매핑을 한 번만 읽어 두 조회에 같은 번호를 쓰게 한다
    std::string readActivePowerText(uint64_t nodeId, bool isManualRequest);
    std::string readCumulativeEnergyText(uint64_t nodeId, bool isManualRequest);
    std::string mChipToolPath;
    Result<CommissionOutcome> commissionDevice(uint64_t nodeId, const std::string& name, const std::string& manualPincode,
                          const std::string& wifiSsid, const std::string& wifiPassword,
                          const std::unordered_set<std::string>& activeDeviceIds);
    // chip-tool 한 번 실행 결과. ok는 "정상 실행·정상 종료(0)"일 때만 true —
    // 시간 초과·실행 실패·비정상 종료는 출력에 성공 표지가 있어도 실패다
    struct ChipToolOutcome {
        bool ok = false;
        bool cancelled = false;  // 서버 종료 등으로 취소됨: 실패지만 기기 탓이 아니다(오프라인 표시 안 함)
        std::string output;
    };
    ChipToolOutcome runChipTool(const std::vector<std::string>& subcommand, std::chrono::milliseconds timeout,
                                const std::vector<std::string>& secretsToMask = {});
    // 제어·조회용: 실패·성공 표지 없음·오류 키워드면 "OFFLINE"(오프라인 명부 갱신), 취소면 "UNKNOWN", 아니면 출력
    std::string runChipToolCommand(const std::vector<std::string>& subcommand, const std::string& deviceId);
    ChipToolRunner& mRunner;
    SetupPinParser mParseSetupPin;

    // 새 커미셔닝에 쓸 노드 ID를 정하고 같은 락 안에서 "페어링 중"으로 표시한다.
    // 시각 기반이되 이전 발급보다 크게, 이미 쓰는 번호는 건너뛴다(같은 초 두 번 커미셔닝해도 겹치지 않음)
    uint64_t reserveNodeIdForPairing();
    // 범위를 벗어나면(예외 포함) "페어링 중" 표시를 반드시 푼다
    class PairingGuard {
    public:
        PairingGuard(MatterController& owner, uint64_t nodeId) : owner_(owner), nodeId_(nodeId) {}
        ~PairingGuard() { owner_.setPairing(nodeId_, false); }
        PairingGuard(const PairingGuard&) = delete;
        PairingGuard& operator=(const PairingGuard&) = delete;
    private:
        MatterController& owner_;
        uint64_t nodeId_;
    };
    uint64_t mLastIssuedNodeId = 0; // stateMutex_
    std::string onDevicePairingComplete(uint64_t nodeId, const std::string& deviceName);
    bool checkDeviceRegistered(uint64_t nodeId);
    bool saveDeviceRegistration(const std::string& serialNumber,uint64_t nodeId);
    // 등록 파일 읽기·쓰기(호출자가 fileMutex_를 잡는다). 쓰기는 임시 파일 → fsync → rename으로 원자적
    std::vector<std::string> readRegistrationLinesLocked();
    bool writeRegistrationLinesLocked(const std::vector<std::string>& lines);
    std::string mConfigFilePath;
    std::string mStorageDirectory;

    // 페어링 중인 기기의 NodeId를 저장
    std::unordered_set<uint64_t> mPairingNodes;

    std::unordered_map<std::string, uint64_t> mSerialToNodeId;
    std::string getDeviceSerialNumber(uint64_t nodeId);
    std::uint64_t getNodeId(const std::string& serialNumber);
    // 오프라인 상태인 기기의 NodeId(문자열)와 타임아웃 발생 시간을 기록
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> mOfflineNodes;

    void setPairing(uint64_t nodeId, bool pairing);
    bool isOffline(const std::string& nodeIdText);
    void markOffline(const std::string& nodeIdText);
    bool clearOffline(const std::string& nodeIdText); // 명부에 있었으면 true

    // 두 락은 동시에 잡지 않는다. chip-tool 실행은 항상 락 밖에서 한다
    std::mutex stateMutex_; // mSerialToNodeId, mOfflineNodes, mPairingNodes
    std::mutex fileMutex_;  // mConfigFilePath 읽기·쓰기
};