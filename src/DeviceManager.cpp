#include "DeviceManager.hpp"
#include "DatabaseManager.hpp"
#include <iostream>
#include <chrono>
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
        if (it->second.state == newState) {
            return true; 
        }
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
        ProtocolType type = it->second.protocol_type;
        auto driverIt = drivers_.find(type);
        bool success = false;
        if (driverIt != drivers_.end()) {
            success = driverIt->second->unpairDevice(id);
        }
        devices_.erase(it);
        DatabaseManager::getInstance().removeDevice(id);
        std::cout << "[DeviceManager] 기기 삭제 완료 : "<< id << std::endl;
        return success;
    }
    std::cerr << "[DeviceManager] 에러 : 기기를 찾을 수 없습니다 (" << id << ")" << std::endl;
    return false;
}
std::string DeviceManager::getDeviceState(std::string id){
    ProtocolType type;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(id);
        if (it == devices_.end()) {
            return "UNKNOWN"; // 기기가 없으면 바로 종료
        }
        type = it->second.protocol_type;
    } 
    auto driverIt = drivers_.find(type);
    if (driverIt != drivers_.end()) {
        std::string realState = driverIt->second->readDeviceState(id);
  
        if (realState != "UNKNOWN") {
            this->updateDeviceState(id, realState); 
        }
        
        return realState;
    }
    
    return "UNKNOWN";
}
void DeviceManager::startHealthCheck() {
    if (!is_running_) {
        is_running_ = true;
        // healthCheckRoutine 함수를 백그라운드 스레드로 분리해서 실행!
        health_thread_ = std::thread(&DeviceManager::healthCheckRoutine, this);
        std::cout << "[DeviceManager] 🛡️ 백그라운드 헬스 체크 데몬 가동 (주기: 60초)" << std::endl;
    }
}

void DeviceManager::stopHealthCheck() {
    if (is_running_) {
        is_running_ = false;
        if (health_thread_.joinable()) {
            health_thread_.join(); // 스레드가 안전하게 끝날 때까지 기다림
        }
        std::cout << "[DeviceManager] 🛡️ 헬스 체크 데몬 종료" << std::endl;
    }
}

void DeviceManager::healthCheckRoutine() {
    while (is_running_) {
        // 🌟 60초 대기 (단, 서버 종료 명령이 오면 1초 단위로 빨리 빠져나올 수 있도록 분할 대기)
        for (int i = 0; i < 60 && is_running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (!is_running_) break;

        std::cout << "\n[DeviceManager] 모든 기기의 상태 점검 시작..." << std::endl;

        std::vector<std::string> deviceIds;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& pair : devices_) {
                deviceIds.push_back(pair.first);
            }
        } 

        for (const auto& id : deviceIds) {
            // 우리가 예전에 최적화해 둔 그 함수! (알아서 통신하고, 알아서 DB 업데이트까지 다 함)
            std::string state = this->getDeviceState(id);
            std::cout << "[DeviceManager] 기기 상태 점검 : " << id << " 현재 상태 ➔ [" << state << "]" << std::endl;
            if (state == "UNKNOWN") {
                std::cerr << "[DeviceManager] 기기 오프라인 : " << id << std::endl;
                // 필요하다면 여기서 DB를 오프라인 상태로 바꾸는 코드를 추가해도 좋습니다.
                DatabaseManager::getInstance().updateDeviceState(id, 0); // 오프라인 상태로 DB 업데이트
            }
        }
        std::cout << "[DeviceManager] 점검 완료.\n" << std::endl;
    }
}