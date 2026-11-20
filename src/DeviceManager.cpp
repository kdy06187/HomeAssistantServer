#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "DatabaseManager.hpp"
#include <iostream>
#include <chrono>
#include <thread>
DeviceManager::DeviceManager(DatabaseManager& db, DriverManager& deviceDrivers, Clock& clock)
    : db_(db), deviceDrivers_(deviceDrivers), clock_(clock) {
    std::cout << "DeviceManager 인스턴스 생성" << std::endl;
}

// ProtocolDriver 등록
void DeviceManager::registerDriver(ProtocolType type, ProtocolDriver* driver){
    std::lock_guard<std::mutex> lock(mutex_);
    drivers_[type] = driver;
    std::cout << "[DeviceManager] 드라이버 등록 완료" << std::endl;
}
void DeviceManager::initFromDatabase(){
    // DB 읽기와 드라이버 조립은 락 밖에서 하고, 맵에 넣을 때만 락을 잡는다
    auto loadedDevices = db_.loadAllDevices();
    const auto driversSnapshot = protocolDriversSnapshot();
    for(auto& dev : loadedDevices){

        // 1. 이 기기가 사용하는 통신 프로토콜 드라이버(메신저) 찾기
        auto driverIt = driversSnapshot.find(dev.protocol_type);
        std::cout << "[DeviceManager] DB에서 기기 로드 중: " << dev.name << " (" << dev.id << ") - 프로토콜: " << getProtocolString(dev.protocol_type) << std::endl;
        if (driverIt != driversSnapshot.end()) {
            std::cout << "[DeviceManager] 기기 로드 중: " << dev.name << " (" << dev.id << ") - 프로토콜: " << getProtocolString(dev.protocol_type) << std::endl;
            IMessageSender* sender = dynamic_cast<IMessageSender*>(driverIt->second);

            // 2. 조립할 부품(driver_types)이 있고, 발송기(sender)가 정상적으로 찾아졌다면 조립!
            if (sender && !dev.driver_types.empty()) {
                deviceDrivers_.attachDriversToDevice(dev, dev.driver_types, sender);
            }
        }
    }
    const size_t loadedCount = loadedDevices.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 3. 조립이 완료된 완성품을 맵에 이동(move)시켜서 등록
        for (auto& dev : loadedDevices) {
            std::string id = dev.id;
            devices_[id] = std::move(dev);
        }
    }
    std::cout << "[DeviceManager] DB에서 기기 로드 완료 (" << loadedCount << "개)" << std::endl;
}
// 기기 추가
bool DeviceManager::addDevice(std::string id, std::string name, ProtocolType protocol_type,
                              const std::vector<DriverType>& driverTypes, IMessageSender* sender){
    Device newDevice(id, name, protocol_type);
    newDevice.state = "UNKNOWN";
    if (sender) {
        newDevice.driver_types = driverTypes;
        deviceDrivers_.attachDriversToDevice(newDevice, driverTypes, sender);
    }
    // DB에 먼저 저장하고 성공했을 때만 메모리에 넣는다. 그래야 실패 시 되돌릴 것이 없고,
    // 다른 스레드가 "곧 사라질 기기"를 보는 틈도 없다. DB 호출은 맵 락 밖에서(L-2)
    if (!db_.insertDevice(newDevice)) {
        std::cerr << "[DeviceManager] 에러 : 기기 DB 저장 실패, 등록하지 않습니다 (" << id << ")" << std::endl;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        devices_[id] = std::move(newDevice);
    }
    std::cout << "[DeviceManager] 기기 추가 완료: " << name << "(" << id << ")" << std::endl;
    return true;
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(id);
        if(it == devices_.end()) return false;
        if (it->second.state == newState) return true;
        it -> second.state = newState;
    }
    // 같은 기기의 동시 갱신이 DB에 역순으로 쓰일 수 있으나 주기 헬스체크가 바로잡는다
    int isActive = (newState == "ON" || newState == "TURN_ON") ? 1 : 0;
    db_.updateDeviceState(id, isActive);
    std::cout << "[DeviceManager] 기기 상태 업데이트 완료: " << id << " -> " << newState << std::endl;
    return true;
}

// 기기 제어
// 반환하는 state는 응답에 그대로 쓰는 "명령 처리 후 상태"다(미등록은 UNKNOWN, 하드웨어를 안 건드렸으면 현재 상태)
CommandResult DeviceManager::executeCommand(std::string id, std::string command){
    Device targetDevice;
    ProtocolDriver* protocolDriver = nullptr;
    // 기기와 프로토콜 드라이버를 락 안에서 복사해 두고, 드라이버 호출은 모두 락 밖에서 한다
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(id);
        if(it == devices_.end()){
            std::cerr << "[DeviceManager] 에러 : 기기를 찾을 수 없습니다 (" << id << ")" << std::endl;
            return CommandResult{CommandOutcome::Failed, Error{ErrorCode::DeviceNotFound, "device not found"}, "UNKNOWN", 0};
        }
        targetDevice = it->second;
        auto driverIt = drivers_.find(targetDevice.protocol_type);
        if (driverIt != drivers_.end()) protocolDriver = driverIt->second;
    }
    DriverResult action = deviceDrivers_.executeDriversForDevice(targetDevice, command);

    const std::string currentState = targetDevice.state;
    if (!action.proceed_hardware) {
        return CommandResult{CommandOutcome::Failed, Error{ErrorCode::CommandBlocked, "blocked by device driver"}, currentState, 0};
    }
    auto execute_hardware = [this, id, command, protocolDriver, currentState]() {
        if (!protocolDriver) {
            std::cerr << "[DeviceManager] 에러 : 프로토콜 드라이버를 찾을 수 없습니다\n";
            return CommandResult{CommandOutcome::Failed, Error{ErrorCode::Internal, "protocol driver not found"}, currentState, 0};
        }
        if (protocolDriver->sendCommand(id, command)) {
            std::string newState = (command == "ON") ? "ON" : "OFF";
            this->updateDeviceState(id, newState); // DB 및 서버 상태 변경
            return CommandResult{CommandOutcome::Succeeded, Error{}, newState, 0};
        }
        this->updateDeviceState(id, "OFFLINE");
        return CommandResult{CommandOutcome::Failed, Error{ErrorCode::DeviceUnreachable, "device did not respond"}, "OFFLINE", 0};
    };

    if (action.delay_seconds > 0) {
        int delay = action.delay_seconds;
        // 서버가 멈추지 않도록 스레드를 생성하여 대기 후 하드웨어 제어
        std::thread([execute_hardware, delay]() {
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            execute_hardware();
        }).detach();
        return CommandResult{CommandOutcome::Accepted, Error{}, currentState, delay};
    }
    // 대기 시간이 없다면 즉시 실행
    return execute_hardware();
}
bool DeviceManager::startCommissioning(ProtocolType type, std::string name, std::string payload
        , const std::string& ssid, const std::string& password, const std::vector<DriverType>& driverTypes){
    std::cout << "[DeviceManager] 커미셔닝 시작: " << getProtocolString(type) << ", " << name << std::endl;
    ProtocolDriver* driver = nullptr;
    std::unordered_set<std::string> idsSnapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto driverIt = drivers_.find(type);
        if (driverIt != drivers_.end()) driver = driverIt->second;
        for (const auto& pair : devices_) idsSnapshot.insert(pair.first);
    }
    if (!driver) {
        std::cerr << "[DeviceManager] 에러 : " << getProtocolString(type) << " 드라이버를 찾을 수 없습니다 " << std::endl;
        return false;
    }

    // 커미셔닝은 수십 초 걸리므로 락 밖에서 하고, 드라이버에는 활성 ID 사본만 넘긴다
    const auto outcome = driver->commission(CommissionRequest{name, payload, ssid, password}, idsSnapshot);
    bool isCommissioned = outcome.ok();
    if (isCommissioned && !outcome.value->deviceId.empty() &&
        !addDevice(outcome.value->deviceId, name, type, driverTypes, dynamic_cast<IMessageSender*>(driver))) {
        // 기기는 페어링됐지만 서버 등록은 실패: 드라이버 쪽 등록(Matter 매핑·등록 파일, TCP 연결)도 정리해
        // "실패 = 아무것도 남지 않음"이 되게 한다. 락 밖에서 부른다
        isCommissioned = false;
        try {
            driver->unpairDevice(outcome.value->deviceId);
        } catch (const std::exception& e) {
            std::cerr << "[DeviceManager] 등록 실패 정리 중 예외 : " << outcome.value->deviceId << " (" << e.what() << ")" << std::endl;
        }
    }
    std::cout << "[DeviceManager] " << getProtocolString(type) << " 커미셔닝 결과: "
              << (isCommissioned ? "성공" : "실패") << std::endl;
    return isCommissioned;
}
// 순서: [락] 맵에서 꺼냄 → DB 삭제(실패 시 되돌림) → 락 없이 해제.
// DB를 먼저 지워, 해제가 실패하거나 예외를 던져도 재시작 뒤 기기가 되살아나지 않게 한다.
bool DeviceManager::removeDevice(std::string id){
    Device removedDevice;
    ProtocolDriver* protocolDriver = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(id);
        if(it == devices_.end()){
            std::cerr << "[DeviceManager] 에러 : 기기를 찾을 수 없습니다 (" << id << ")" << std::endl;
            return false;
        }
        auto driverIt = drivers_.find(it->second.protocol_type);
        if (driverIt != drivers_.end()) protocolDriver = driverIt->second;
        // 맵에서 먼저 빼서, 삭제가 진행되는 동안 들어온 요청은 "없는 기기"로 처리되게 한다
        removedDevice = std::move(it->second);
        devices_.erase(it);
    }
    if (!db_.removeDevice(id)) {
        std::cerr << "[DeviceManager] 에러 : DB 삭제 실패, 기기를 되돌립니다 (" << id << ")" << std::endl;
        std::lock_guard<std::mutex> lock(mutex_);
        // 그 사이 같은 ID가 다시 등록됐다면 새 기기를 덮어쓰지 않는다
        devices_.emplace(id, std::move(removedDevice));
        return false;
    }
    // 드라이버 해제는 자기 락(소켓·파일)을 잡으므로 맵 락을 쥔 채 부르면 교착 위험이 있다.
    // DB에서 지웠으면 삭제는 끝난 것이다. 오프라인 기기는 해제가 실패하는 게 보통이라 로그만 남긴다
    if (protocolDriver && !protocolDriver->unpairDevice(id)) {
        std::cerr << "[DeviceManager] 경고 : 드라이버 해제 실패, 등록 정보는 삭제됨 (" << id << ")" << std::endl;
    }
    std::cout << "[DeviceManager] 기기 삭제 완료 : "<< id << std::endl;
    return true;
}
std::string DeviceManager::getDeviceState(std::string id, bool isManualRefresh) {
    ProtocolDriver* protocolDriver = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(id);
        if (it == devices_.end()) {
            return "UNKNOWN"; // 기기가 없으면 바로 종료
        }
        auto driverIt = drivers_.find(it->second.protocol_type);
        if (driverIt != drivers_.end()) protocolDriver = driverIt->second;
    }
    if (protocolDriver) {
        std::string realState = protocolDriver->readDeviceState(id, isManualRefresh); // 통신은 락 밖
  
        if (realState != "UNKNOWN") {
            this->updateDeviceState(id, realState); 
        }
        
        return realState;
    }
    
    return "UNKNOWN";
}
void DeviceManager::runHealthCheck() {
    std::cout << "\n[DeviceManager] 모든 기기의 상태 점검 시작..." << std::endl;

    const std::vector<Device> devices = getAllDevices();

    for (const auto& device : devices) {
        try {
            const auto& id = device.id;
            // 우리가 예전에 최적화해 둔 그 함수! (알아서 통신하고, 알아서 DB 업데이트까지 다 함)
            std::string state = this->getDeviceState(id);
            std::cout << "[DeviceManager] 기기 상태 점검 : " << id << " 현재 상태 ➔ [" << state << "]" << std::endl;
            if (state == "UNKNOWN" || state == "OFFLINE") {
                // OFFLINE 저장은 getDeviceState가 이미 했다. UNKNOWN은 "모름"이라 저장된 상태를 바꾸지 않는다
                std::cerr << "[DeviceManager] 기기 오프라인 : " << id << std::endl;
            }
            // 상태를 확인하지 못한 기기(UNKNOWN·OFFLINE·PAIRING·ERROR)는 전력 조회도 건너뛴다
            const bool stateConfirmed = state != "UNKNOWN" && state != "OFFLINE" && state != "PAIRING" && state != "ERROR";
            if (device.protocol_type == ProtocolType::MATTER && stateConfirmed) {
                const auto energy = readEnergy(id);
                if (energy.ok() && db_.insertEnergyLog(id, energy.value->totalEnergy_mWh)) {
                    std::cout << "[DeviceManager] 📊 기기(" << id << ") 시계열 데이터 기록: " << energy.value->totalEnergy_mWh << " mWh" << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[DeviceManager] 기기 점검 중 예외 : " << device.id << " (" << e.what() << ")" << std::endl;
        } catch (...) {
            std::cerr << "[DeviceManager] 기기 점검 중 알 수 없는 예외 : " << device.id << std::endl;
        }
    }
    std::cout << "[DeviceManager] 점검 완료.\n" << std::endl;
}

std::unordered_set<std::string> DeviceManager::activeDeviceIds() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_set<std::string> ids;
    for (const auto& pair : devices_) ids.insert(pair.first);
    return ids;
}

Result<EnergySnapshot> DeviceManager::readEnergy(const std::string& deviceId) {
    ProtocolDriver* protocolDriver = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(deviceId);
        if (it == devices_.end()) {
            std::cerr << "[DeviceManager] 전력 조회 실패: 등록되지 않은 기기 (" << deviceId << ")" << std::endl;
            return Result<EnergySnapshot>::failure(ErrorCode::DeviceNotFound, "device not found");
        }
        // 기기 자신의 프로토콜 드라이버에 묻는다. 전력 조회를 지원하지 않는 드라이버는 실패를 돌려준다
        auto driverIt = drivers_.find(it->second.protocol_type);
        if (driverIt != drivers_.end()) protocolDriver = driverIt->second;
    }
    if (!protocolDriver) {
        std::cerr << "[DeviceManager] 전력 조회 실패: 프로토콜 드라이버 없음 (" << deviceId << ")" << std::endl;
        return Result<EnergySnapshot>::failure(ErrorCode::Internal, "protocol driver not found");
    }

    // 같은 기기 조회끼리만 기다린다. mutex_는 쥐지 않은 상태다
    const auto deviceEnergyLock = energyLockFor(deviceId);
    std::lock_guard<std::mutex> energyLock(*deviceEnergyLock);

    const auto reading = protocolDriver->readEnergy(deviceId); // chip-tool 실행 중이므로 상태 락 밖
    if (!reading.ok()) {
        std::cerr << "[DeviceManager] 기기(" << deviceId << ") 전력 조회 실패: " << reading.error.message << std::endl;
        // 기기 존재는 위에서 확인했다. 드라이버의 "모름"(매핑 없음)은 내부 불일치로 본다
        const ErrorCode code = reading.error.code == ErrorCode::DeviceNotFound ? ErrorCode::Internal : reading.error.code;
        return Result<EnergySnapshot>::failure(code, reading.error.message);
    }

    // 조회 도중 삭제됐으면 통계 행을 만들지 않도록 저장 전에 다시 확인한다
    EnergySnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(deviceId);
        if (it == devices_.end()) return Result<EnergySnapshot>::failure(ErrorCode::DeviceNotFound, "device removed");
        snapshot.deviceState = it->second.state;
    }

    // 조회에 성공한 값만 통계에 반영한다. 실패를 0으로 저장하면 다음 정상 조회 때 누적값 전체가 더해진다
    snapshot.activePower_mW = reading.value->activePower_mW;
    if (!db_.updateEnergyStat(deviceId, reading.value->cumulative_mWh, snapshot.totalEnergy_mWh)) {
        return Result<EnergySnapshot>::failure(ErrorCode::Internal, "energy stat update failed");
    }
    return Result<EnergySnapshot>::success(snapshot);
}

Result<HistoryResult> DeviceManager::getEnergyHistory(const std::string& deviceId, int limit) {
    HistoryResult history;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(deviceId);
        if (it == devices_.end()) {
            return Result<HistoryResult>::failure(ErrorCode::DeviceNotFound, "device not found");
        }
        history.deviceState = it->second.state;
    }
    auto logs = db_.getEnergyHistory(deviceId, limit);
    if (!logs.ok()) return Result<HistoryResult>::failure(logs.error.code, logs.error.message);
    history.logs = std::move(*logs.value);
    return Result<HistoryResult>::success(std::move(history));
}

std::unordered_map<ProtocolType, ProtocolDriver*> DeviceManager::protocolDriversSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return drivers_;
}

std::shared_ptr<std::mutex> DeviceManager::energyLockFor(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(energyLocksMutex_);
    auto& slot = energyLocks_[deviceId];
    if (!slot) slot = std::make_shared<std::mutex>();
    return slot;
}
