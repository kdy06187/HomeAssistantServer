#pragma once
#include <unordered_map>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <iostream>
#include "DriverType.hpp"
#include "IDeviceDriver.hpp"
#include "IMessageSender.hpp" // ProtocolDriver 대신 가벼운 인터페이스 사용
#include "Device.hpp"

class DriverManager {
public:
    DriverManager() = default;
    DriverManager(const DriverManager&) = delete;
    DriverManager& operator=(const DriverManager&) = delete;

    // 드라이버 생성 함수 규격: IMessageSender를 주입받아 특수 드라이버 객체를 생성
    using DriverCreator = std::function<std::shared_ptr<IDeviceDriver>(IMessageSender*)>;

    // 1. 드라이버 생성 레시피 등록 (main이 부팅 때 호출)
    void registerDriver(DriverType type, DriverCreator creator) {
        creators_[type] = creator;
    }

    // 2. 기기에 알맞은 특수 드라이버 객체들을 생성하여 Device 내부의 배열(주머니)에 부착
    void attachDriversToDevice(Device& device, const std::vector<DriverType>& driverTypes, IMessageSender* messageSender) {
        for (DriverType type : driverTypes) {
            auto it = creators_.find(type);
            if (it != creators_.end()) {
                // 생성된 드라이버 객체를 기기의 attached_drivers 벡터에 추가
                device.attached_drivers.push_back(it->second(messageSender));
            } else {
                std::cerr << "[DriverManager] ⚠️ 등록되지 않은 드라이버 타입 번호입니다: " << static_cast<int>(type) << "\n";
            }
        }
        std::cout << "[DriverManager] 기기 [" << device.name << "]에 " << device.attached_drivers.size() << "개의 드라이버 부착 완료" << std::endl;
    }

    // 3. 기기에 부착된 드라이버들을 순회하며 제어 명령 실행을 지시
    DriverResult executeDriversForDevice(const Device& device, const std::string& command) {
        DriverResult finalResult = { true, 0 };

        for (const auto& driver : device.attached_drivers) {
            if (driver) {
                DriverResult res = driver->handleCommand(device, command);
                
                // 하나라도 하드웨어 제어를 막는다면 최종 결과도 false
                if (!res.proceed_hardware) {
                    finalResult.proceed_hardware = false;
                }
                // 가장 긴 대기 시간을 적용
                if (res.delay_seconds > finalResult.delay_seconds) {
                    finalResult.delay_seconds = res.delay_seconds;
                }
            }
        }
        return finalResult;
    }   

private:
    // 번호(Enum)와 생성 함수(레시피)를 연결해두는 내부 저장소
    std::unordered_map<DriverType, DriverCreator> creators_;
};
