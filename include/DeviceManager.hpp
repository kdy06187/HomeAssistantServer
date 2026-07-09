#pragma once
#include <string>
#include <iostream>
#include <unordered_map>
#include <mutex>
#include "Device.hpp"
#include "ProtocolDriver.hpp"

class DeviceManager {
public:
// DeviceManager의 싱글톤 인스턴스를 반환하는 정적 메서드
    static DeviceManager& getInstance(){
        static DeviceManager instance;
        return instance;
    }
    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    // 시스템 초기화 API - 프로토콜 타입별로 어떤 드라이버를 쓸지 등록
    void registerDriver(ProtocolType type, ProtocolDriver* driver);

    // 디바이스 등록 API - 디바이스를 등록하고, 프로토콜 타입에 맞는 드라이버를 통해 초기화
    void addDevice(std::string id, std::string name, ProtocolType protocol_type);
    bool getDevice(std::string id, Device& outDevice);
    bool updateDeviceState(std::string id, std::string newState);

    // 디바이스 제어 API - 디바이스 ID와 명령을 받아 해당 디바이스를 제어
    bool executeCommand(std::string id, std::string command);
private:
    // 외부 생성 방지
    DeviceManager(){
        std::cout << "DeviceManager 인스턴스 생성" << std::endl;
    }
    ~DeviceManager() = default;

    std::unordered_map<std::string,Device> devices_;
    std::unordered_map<ProtocolType, ProtocolDriver*> drivers_;
    std::mutex mutex_;
    

};