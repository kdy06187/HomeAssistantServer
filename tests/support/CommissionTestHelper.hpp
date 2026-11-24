#pragma once
#include <atomic>
#include <string>
#include <vector>
#include "CommissioningService.hpp"

// 작업 스레드 없이 커미셔닝 한 건을 동기로 실행한다(작업 스레드가 부르는 commissionOnce 그대로).
// cancelRequested를 넘기지 않으면 취소되지 않은 것으로 본다
inline bool commissionVia(DeviceManager& manager, ProtocolDriver* driver, ProtocolType type, const std::string& name,
                          const std::string& payload, const std::string& ssid = "", const std::string& password = "",
                          const std::vector<DriverType>& driverTypes = {},
                          const std::atomic<bool>* cancelRequested = nullptr) {
    static const std::atomic<bool> kNotCancelled{false};
    return CommissioningService::commissionOnce(manager, driver, type, CommissionRequest{name, payload, ssid, password},
                                                driverTypes, cancelRequested ? *cancelRequested : kNotCancelled)
        .ok();
}
