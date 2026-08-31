#pragma once
#include <string>
class DeviceManager;
class ProtocolDriver {
protected:
    DeviceManager& mDeviceManager;
public:
    ProtocolDriver(DeviceManager& deviceManager) : mDeviceManager(deviceManager) {} 
    // ProtocolDriver의 가상 소멸자
    virtual ~ProtocolDriver() = default;

    virtual bool sendCommand(std::string deviceId, std::string command) = 0;
    virtual bool commissionDevice(std::string name, std::string payload) = 0;
    virtual bool unpairDevice(std::string deviceId) = 0;
    virtual std::string readDeviceState(std::string deviceId, bool isManualRequest = false) = 0;
};