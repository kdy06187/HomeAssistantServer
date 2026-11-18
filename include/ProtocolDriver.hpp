#pragma once
#include "Result.hpp"
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
    // 비어 있으면 이미 등록된 기기라 호출자가 등록할 것이 없다는 뜻
    std::string deviceId;
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
    virtual Result<CommissionOutcome> commission(const CommissionRequest& request,
                                                 const std::unordered_set<std::string>& activeDeviceIds) = 0;
    virtual bool unpairDevice(const std::string& deviceId) = 0;
    virtual std::string readDeviceState(const std::string& deviceId, bool isManualRequest) = 0;

    virtual Result<EnergyReading> readEnergy(const std::string& /*deviceId*/) {
        return Result<EnergyReading>::failure(ErrorCode::UnsupportedOperation, "전력 조회를 지원하지 않는 드라이버");
    }
};
