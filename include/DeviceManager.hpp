#pragma once
#include <string>
#include <iostream>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <thread>
#include <atomic>
#include "Device.hpp"
#include "ProtocolDriver.hpp"
#include "DatabaseManager.hpp"

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

    void initFromDatabase();
    // 디바이스 등록 API - 디바이스를 등록하고, 프로토콜 타입에 맞는 드라이버를 통해 초기화
    void addDevice(std::string id, std::string name, ProtocolType protocol_type);
    bool getDevice(std::string id, Device& outDevice);
    std::string getDeviceState(std::string id);
    std::vector<Device> getAllDevices();
    std::string getProtocolString(ProtocolType type);
    bool updateDeviceState(std::string id, std::string newState);
    bool startCommissioning(ProtocolType type, std::string name, std::string payload);
    // 디바이스 제어 API - 디바이스 ID와 명령을 받아 해당 디바이스를 제어
    bool executeCommand(std::string id, std::string command);
    bool removeDevice(std::string id);
    bool hasDevice(std::string id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return devices_.find(id) != devices_.end();
    }
    void startHealthCheck();
    void stopHealthCheck();

    std::string getDeviceActivePower(std::string deviceId);
    std::string getDeviceTotalEnergy(std::string deviceId);
    bool getDeviceEnergyInfo(const std::string& deviceId, int& out_activePower_mW,long& out_real_total_mWh);
    std::vector<EnergyLog> getDeviceEnergyHistory(const std::string& deviceId, int limit = 50);
private:
    // 외부 생성 방지
    DeviceManager(){
        std::cout << "DeviceManager 인스턴스 생성" << std::endl;
    }
    ~DeviceManager() = default;

    std::thread health_thread_;
    std::atomic<bool> is_running_{false};
    void healthCheckRoutine();

    std::unordered_map<std::string,Device> devices_;
    std::unordered_map<ProtocolType, ProtocolDriver*> drivers_;
    std::mutex mutex_;
    
};