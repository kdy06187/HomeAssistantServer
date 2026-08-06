#pragma once
#include <string>

class ProtocolDriver {
public:
    // ProtocolDriver의 가상 소멸자
    virtual ~ProtocolDriver() = default;

    virtual bool sendCommand(std::string deviceId, std::string command) = 0;
};