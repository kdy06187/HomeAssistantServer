#include "PCDriver.hpp"
#include "IMessageSender.hpp"
#include "DriverManager.hpp"
#include "Device.hpp"
#include <iostream>
#include <thread>
#include <chrono>

PCDriver::PCDriver(IMessageSender* sender) : pc_messenger_(sender) {}

DriverResult PCDriver::handleCommand(const Device& device, const std::string& command){
    std::cout << "[PCDriver] 명령 수신: " << command << " (Device ID: " << device.id << ")\n";
    // OFF 명령일 때만 가로채서 특수 시퀀스 실행
    if (command == "OFF") {
        std::cout << "[PCDriver] PC 끄기 시퀀스 시작 (Device ID: " << device.id << ")\n";
        if (pc_messenger_) {
            std::string payload = R"({
                "action": "SYSTEM",
                "target": "OS",
                "value": "SHUTDOWN"
            })";
    
            pc_messenger_->SendCommandTo("PC_PYTHON_CLIENT", payload);
        }   

        return { true, 45 };

    }

    return { true, 0 };
}
namespace {
    const DriverRegistrar registrar(DriverType::PC_DRIVER, [](IMessageSender* m) {
        return std::make_shared<PCDriver>(m);
    });
}