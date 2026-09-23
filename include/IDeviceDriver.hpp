#pragma once
#include "Device.hpp"
#include <string>
// 순환 참조(Circular Reference) 방지를 위한 전방 선언
class Device;

struct DriverResult {
    bool proceed_hardware; // 다음 단계(하드웨어 프로토콜 제어) 진행 여부
    int delay_seconds;     // 하드웨어 제어 전 대기할 시간 (초, 0이면 즉시 실행)
};

class IDeviceDriver {
public:
    virtual ~IDeviceDriver() = default;

    // 디바이스 명령을 처리하는 가상 함수
    virtual DriverResult handleCommand(const Device& device, const std::string& command) = 0;
};