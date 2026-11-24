#pragma once
#include "Result.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>

struct CommissionRequest {
    std::string name;
    std::string payload;
    std::string ssid;
    std::string password; // 로그에 남기지 않는다
};

struct CommissionOutcome {
    std::string deviceId;
    // 등록 시점의 접속 IP(TCP만, 모르면 빈 문자열). 등록 때 함께 저장한다
    std::string ip;
    // Matter만: 이번에 페어링한 노드 번호(0 = 없음). 등록 확정 훅이 매핑을 쓰거나 이 노드만 정리하는 데 쓴다
    uint64_t nodeId = 0;

    // ip를 생략한 CommissionOutcome{"ID"}(Matter·기존 호출)가 그대로 되도록 생성자를 둔다
    CommissionOutcome() = default;
    CommissionOutcome(std::string id, std::string connectionIp = "", uint64_t pairedNodeId = 0)
        : deviceId(std::move(id)), ip(std::move(connectionIp)), nodeId(pairedNodeId) {}
};

struct EnergyReading {
    int64_t activePower_mW;
    int64_t cumulative_mWh;
};

class ProtocolDriver {
public:
    virtual ~ProtocolDriver() = default;

    virtual bool sendCommand(const std::string& deviceId, const std::string& command) = 0;
    // activeDeviceIds는 호출 시점의 복사본이므로 호출자 락과 무관하게 읽을 수 있다. 기기 등록은 호출자가 한다.
    // cancelRequested가 서면(서버 종료) 가능한 빨리 Cancelled로 끝낸다. 호출이 끝날 때까지 살아 있어야 한다
    virtual Result<CommissionOutcome> commission(const CommissionRequest& request,
                                                 const std::unordered_set<std::string>& activeDeviceIds,
                                                 const std::atomic<bool>& cancelRequested) = 0;
    // commission 성공 뒤 서버 등록 결과를 알린다(DeviceManager가 그 기기 ID 등록 락 안·상태 락 밖에서 부른다).
    // accepted=true: 드라이버 쪽 등록을 확정한다(Matter 매핑·등록 파일). false: 이번 commission이 만든 것만 정리한다
    // (Matter는 새 노드만 unpair, 같은 시리얼의 기존 매핑은 그대로). 기존 등록을 commission 안에서 덮어쓰지 않으려고 나눴다
    virtual void finalizeCommission(const CommissionOutcome& /*outcome*/, bool /*accepted*/) {}
    virtual bool unpairDevice(const std::string& deviceId) = 0;
    virtual std::string readDeviceState(const std::string& deviceId, bool isManualRequest) = 0;

    virtual Result<EnergyReading> readEnergy(const std::string& /*deviceId*/) {
        return Result<EnergyReading>::failure(ErrorCode::UnsupportedOperation, "전력 조회를 지원하지 않는 드라이버");
    }
};
