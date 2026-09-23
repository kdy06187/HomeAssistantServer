#include "PCDriver.hpp"
#include "Device.hpp"
#include <iostream>
#include <thread>
#include <chrono>

PCDriver::PCDriver(ProtocolDriver* tcp_driver) : tcp_driver_(tcp_driver) {}

DriverResult PCDriver::handleCommand(const Device& device, const std::string& command) override {
    // OFF 명령일 때만 가로채서 특수 시퀀스 실행
    if (command == "OFF") {
        std::cout << "[PCDriver] PC 끄기 시퀀스 시작 (Device ID: " << device.id << ")\n";
        if (pc_messenger_) {
            std::string payload = R"({
                "action": "SYSTEM",
                "target": "OS",
                "value": "SHUTDOWN"
            })";
    
            pc_messenger_->sendCommandToTarget("PC_PYTHON_CLIENT", payload);
        }   

        return { true, 45 };

    }

    return { true, 0 };
}