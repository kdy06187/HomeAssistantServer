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
#include <mutex>

class DriverManager {
public:
    // 드라이버 생성 함수 규격: IMessageSender를 주입받아 특수 드라이버 객체를 생성
    using DriverCreator = std::function<std::shared_ptr<IDeviceDriver>(IMessageSender*)>;

    static DriverManager& getInstance() {
        static DriverManager instance;
        return instance;
    }

    // 1. 드라이버 생성 레시피 등록 (각 드라이버 .cpp 파일에서 자기 자신을 등록할 때 사용)
    void registerDriver(DriverType type, DriverCreator creator) {
        creators_[type] = creator;
    }

    void reserveDrivers(const std::string& deviceName, const std::vector<DriverType>& drivers) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_drivers_[deviceName] = drivers;
    }

    // 기기 등록 완료 시: 예약된 드라이버를 꺼내어 최종 부착 (DeviceManager가 호출)
    void attachReservedDrivers(Device& device, const std::string& name, IMessageSender* sender) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_drivers_.find(name);
        
        if (it != pending_drivers_.end()) {
            device.driver_types = it->second;
            // 기존에 만들어둔 순수 조립 함수를 재활용
            attachDriversToDevice(device, it->second, sender); 
            // 조립이 끝났으므로 예약 명단에서 삭제
            pending_drivers_.erase(it);
            std::cout << "[DriverManager] 기기 [" << name << "] 드라이버 부착 완료" << std::endl;
        }
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
    DriverManager() = default;
    ~DriverManager() = default;
    DriverManager(const DriverManager&) = delete;
    DriverManager& operator=(const DriverManager&) = delete;

    // 번호(Enum)와 생성 함수(레시피)를 연결해두는 내부 저장소
    std::unordered_map<DriverType, DriverCreator> creators_;

    std::mutex mutex_;
    std::unordered_map<std::string, std::vector<DriverType>> pending_drivers_;
};

// 🌟 [자가 등록 도우미 구조체]
// 이 구조체를 각 드라이버 소스 파일(.cpp)에서 전역 변수로 선언하면,
// main() 함수가 실행되기 전에 자동으로 DriverManager에 레시피가 등록됩니다.
struct DriverRegistrar {
    DriverRegistrar(DriverType type, DriverManager::DriverCreator creator) {
        DriverManager::getInstance().registerDriver(type, creator);
    }
};