#include "DeviceManager.hpp"
#include "DatabaseManager.hpp"
#include <iostream>
// ProtocolDriver 등록
void DeviceManager::registerDriver(ProtocolType type, ProtocolDriver* driver){
    std::lock_guard<std::mutex> lock(mutex_);
    drivers_[type] = driver;
    std::cout << "[DeviceManager] 드라이버 등록 완료" << std::endl;
}
void DeviceManager::initFromDatabase(){
    std::lock_guard<std::mutex> lock(mutex_);
    auto loadedDevices = DatabaseManager::getInstance().loadAllDevices();
    for(const auto& dev : loadedDevices){
        devices_[dev.id] = dev;
    }
    std::cout << "[DeviceManager] DB에서 기기 로드 완료 (" << loadedDevices.size() << "개)" << std::endl;
}
// 기기 추가
void DeviceManager::addDevice(std::string id, std::string name, ProtocolType protocol_type){
    std::lock_guard<std::mutex> lock(mutex_);
    Device newDevice = {id,name,protocol_type,"UNKNOWN"};
    devices_[id] = newDevice;
    DatabaseManager::getInstance().insertDevice(newDevice);
    std::cout << "[DeviceManager] 기기 추가 완료: " << name << "(" << id << ")" << std::endl;
}

//기기 정보 조회
bool DeviceManager::getDevice(std::string id, Device& outDevice){
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(id);
    if(it != devices_.end()){
        outDevice = it -> second;
        return true;
    }
    return false;
}
std::vector<Device> DeviceManager::getAllDevices() {
    std::lock_guard<std::mutex> lock(mutex_); // 읽는 동안 다른 스레드가 수정 못하게 보호!
    
    std::vector<Device> deviceList;
    deviceList.reserve(devices_.size()); // 성능 최적화: 메모리 미리 할당
    
    for (const auto& pair : devices_) {
        deviceList.push_back(pair.second);
    }
    
    return deviceList;
}
std::string DeviceManager::getProtocolString(ProtocolType type) {
    switch (type) {
        case ProtocolType::MATTER: return "MATTER";
        case ProtocolType::TCP_DIY: return "TCP_DIY";
        default: return "UNKNOWN";
    }
}
//기기 상태 업데이트
bool DeviceManager::updateDeviceState(std::string id, std::string newState){
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(id);
    if(it != devices_.end()){
        it -> second.state = newState;
        int isActive = (newState == "ON" || newState == "TURN_ON") ? 1 : 0;
        DatabaseManager::getInstance().updateDeviceState(id, isActive);
        std::cout << "[DeviceManager] 기기 상태 업데이트 완료: " << id << " -> " << newState << std::endl;
        return true;
    }
    return false;
}

// 기기 제어
bool DeviceManager::executeCommand(std::string id, std::string command){
    ProtocolType deviceType;
    // 기기 존재 여부 확인 및 프로토콜 타입 조회
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(id);
        if(it == devices_.end()){
            std::cerr << "[DeviceManager] 에러 : 기기를 찾을 수 없습니다 (" << id << ")" << std::endl;
            return false;
        } 
        deviceType = it -> second.protocol_type;
    }// 자동으로 mutex 해제

    // 해당 프로토콜 타입에 맞는 드라이버 조회
    auto driverIt = drivers_.find(deviceType);
    if(driverIt != drivers_.end()){
        driverIt->second->sendCommand(id,command);
        return true;
    } else{
        std::cerr << "[DeviceManager] 에러 : 해당 프로토콜 타입에 맞는 드라이버를 찾을 수 없습니다 " << std::endl;

    }
    
    return false;
}
bool DeviceManager::startCommissioning(ProtocolType type, std::string name, std::string payload){
    std::cout << "[DeviceManager] 커미셔닝 시작: " << getProtocolString(type) << ", " << name << ", " << payload << std::endl;
    auto driverIt = drivers_.find(type);
    if(driverIt != drivers_.end()){
        driverIt->second->commissionDevice(name, payload);
        std::cout << "[DeviceManager] " << getProtocolString(type) << " 커미셔닝 요청 완료" << std::endl;
        return true;
    } else{
        std::cerr << "[DeviceManager] 에러 : " << getProtocolString(type) << " 드라이버를 찾을 수 없습니다 " << std::endl;
        return false;
    }
}
bool DeviceManager::removeDevice(std::string id){
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(id);
    if(it != devices_.end()){
        devices_.erase(it);
        DatabaseManager::getInstance().removeDevice(id);
        std::cout << "[DeviceManager] 기기 삭제 완료 : "<< id << std::endl;
        return true;
    }
    std::cerr << "[DeviceManager] 에러 : 기기를 찾을 수 없습니다 (" << id << ")" << std::endl;
    return false;
}

