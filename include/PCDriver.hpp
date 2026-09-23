#pragma once
#include "IDeviceDriver.hpp"
#include "ProtocolDriver.hpp" // 기존 통신 추상화 클래스 활용
#include <memory>
#include <string>

class PCDriver : public IDeviceDriver {
private:
    IMessageSender* pc_messenger_;
public:
    PCDriver(IMessageSender* pc_messenger);
    // OnTurnOff 대신 단일 진입점 사용: 
    // 드라이버가 명령을 가로챘다면 true, 관여하지 않는다면 false를 반환
    DriverResult handleCommand(const Device& device, const std::string& command) override;
};