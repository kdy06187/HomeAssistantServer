#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "DatabaseManager.hpp"
#include "UtcTime.hpp"
#include "RequestValidation.hpp"
#include <algorithm>
#include <iostream>
#include <chrono>
#include <thread>
DeviceManager::DeviceManager(DatabaseManager& db, DriverManager& deviceDrivers,
                             DelayedCommandScheduler& scheduler, Clock& clock)
    : db_(db), deviceDrivers_(deviceDrivers), scheduler_(scheduler), clock_(clock) {
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
    auto loadedDevices = db_.loadActiveDevices();
    for(auto& dev : loadedDevices){
        std::cout << "[DeviceManager] DB에서 기기 로드 중: " << dev.name << " (" << dev.id << ") - 프로토콜: " << getProtocolString(dev.protocol_type) << std::endl;
        // 1차 ID(ARD_<IP>)는 자동으로 옮기지 않는다. 2차 서버에서는 그 ID로 연결되는 기기가 없다
        if (dev.protocol_type == ProtocolType::TCP_DIY && dev.id.rfind("ARD_", 0) == 0) {
            std::cerr << "[DeviceManager] 옛 TCP 기기 ID(" << RequestValidation::sanitizeForLog(dev.id)
                      << ") — 2차에서는 연결되지 않음, 삭제 후 재등록 필요" << std::endl;
        }
        // 기기 드라이버는 프로토콜과 무관하게 붙인다(발송기는 팩토리가 쥐고 있다)
        if (!dev.driver_types.empty()) {
            deviceDrivers_.attachDriversToDevice(dev, dev.driver_types);
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
                              const std::vector<DriverType>& driverTypes, const std::string& ip){
    auto saved = saveDevice(id, name, protocol_type, driverTypes, ip);
    if (!saved) return false;
    publishDevice(std::move(*saved));
    std::cout << "[DeviceManager] 기기 추가 완료: " << name << "(" << id << ")" << std::endl;
    return true;
}

std::optional<Device> DeviceManager::saveDevice(const std::string& id, const std::string& name, ProtocolType protocol_type,
                                                const std::vector<DriverType>& driverTypes, const std::string& ip) {
    Device newDevice(id, name, protocol_type);
    newDevice.state = "UNKNOWN";
    newDevice.ip = ip;
    newDevice.driver_types = driverTypes;
    deviceDrivers_.attachDriversToDevice(newDevice, driverTypes);
    // DB에 먼저 저장하고 성공했을 때만 메모리에 넣는다. 그래야 실패 시 되돌릴 것이 없고,
    // 다른 스레드가 "곧 사라질 기기"를 보는 틈도 없다. DB 호출은 맵 락 밖에서(L-2)
    if (!db_.insertDevice(newDevice)) {
        std::cerr << "[DeviceManager] 에러 : 기기 DB 저장 실패, 등록하지 않습니다 (" << id << ")" << std::endl;
        return std::nullopt;
    }
    // ip 없이 재등록하면 DB는 저장된 ip를 지킨다(COALESCE). 메모리도 그 값으로 맞춰 E1과 DB가 어긋나지 않게 한다
    if (newDevice.ip.empty() && protocol_type == ProtocolType::TCP_DIY) newDevice.ip = db_.loadDeviceIp(id);
    return newDevice;
}

void DeviceManager::publishDevice(Device device) {
    const std::string id = device.id;
    std::lock_guard<std::mutex> lock(mutex_);
    devices_[id] = std::move(device);
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
    const std::string currentState = targetDevice.state;

    // 종료 중에는 기기 드라이버(PC 종료 메시지 발송)도 부르지 않는다. 메시지만 가고 전원 차단이 취소되는 일을 막는다
    if (isShuttingDown_) {
        return CommandResult{CommandOutcome::Failed, Error{ErrorCode::Cancelled, "server shutting down"}, currentState, 0};
    }

    // 같은 명령이 이미 대기 중이면 기기 드라이버를 다시 부르지 않는다(PC 종료 메시지가 한 번만 가도록)
    const auto pendingCommand = scheduler_.pending(id);
    if (pendingCommand && pendingCommand->command == command) {
        return CommandResult{CommandOutcome::Accepted, Error{}, currentState, pendingCommand->remainingSeconds};
    }
    if (pendingCommand) scheduler_.cancel(id, "다른 명령");

    DriverResult action = deviceDrivers_.executeDriversForDevice(targetDevice, command);
    if (!action.proceed_hardware) {
        return CommandResult{CommandOutcome::Failed, Error{ErrorCode::CommandBlocked, "blocked by device driver"}, currentState, 0};
    }

    if (action.delay_seconds > 0) {
        const bool isScheduled = scheduler_.schedule(id, command, std::chrono::seconds(action.delay_seconds),
            [this, id, command, protocolDriver, currentState]() {
                // 예약 뒤 삭제됐다면 보내지 않는다
                if (!isRegistered(id)) {
                    std::cerr << "[DeviceManager] 지연 명령 만기에 기기가 이미 삭제됨, 실행하지 않음 (" << id << ")" << std::endl;
                    return;
                }
                executeHardware(id, command, protocolDriver, currentState);
            });
        if (isScheduled) {
            // 위 기기 복사와 schedule 사이에 삭제됐다면 removeDevice의 cancel이 예약보다 먼저 지나갔을 수 있다
            if (!isRegistered(id)) {
                scheduler_.cancel(id, "삭제");
                return CommandResult{CommandOutcome::Failed, Error{ErrorCode::DeviceNotFound, "device not found"}, "UNKNOWN", 0};
            }
            return CommandResult{CommandOutcome::Accepted, Error{}, currentState, action.delay_seconds};
        }
        // 동시에 들어온 같은 기기 요청이 먼저 예약했거나 스케줄러가 멈춘 경우
        const auto existing = scheduler_.pending(id);
        if (existing) return CommandResult{CommandOutcome::Accepted, Error{}, currentState, existing->remainingSeconds};
        return CommandResult{CommandOutcome::Failed, Error{ErrorCode::Cancelled, "scheduler stopped"}, currentState, 0};
    }
    return executeHardware(id, command, protocolDriver, currentState);
}

bool DeviceManager::isRegistered(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_.count(id) > 0;
}

CommandResult DeviceManager::executeHardware(const std::string& id, const std::string& command,
                                             ProtocolDriver* protocolDriver, const std::string& currentState) {
    if (!protocolDriver) {
        std::cerr << "[DeviceManager] 에러 : 프로토콜 드라이버를 찾을 수 없습니다\n";
        return CommandResult{CommandOutcome::Failed, Error{ErrorCode::Internal, "protocol driver not found"}, currentState, 0};
    }
    if (isShuttingDown_) {
        return CommandResult{CommandOutcome::Failed, Error{ErrorCode::Cancelled, "server shutting down"}, currentState, 0};
    }
    if (protocolDriver->sendCommand(id, command)) {
        std::string newState = (command == "ON") ? "ON" : "OFF";
        updateDeviceState(id, newState); // DB 및 서버 상태 변경
        return CommandResult{CommandOutcome::Succeeded, Error{}, newState, 0};
    }
    // 종료 중의 실패는 취소된 호출일 수 있다. OFFLINE으로 기록하면 재시작 뒤 DB 상태가 틀어진다
    if (isShuttingDown_) {
        return CommandResult{CommandOutcome::Failed, Error{ErrorCode::Cancelled, "server shutting down"}, currentState, 0};
    }
    updateDeviceState(id, "OFFLINE");
    return CommandResult{CommandOutcome::Failed, Error{ErrorCode::DeviceUnreachable, "device did not respond"}, "OFFLINE", 0};
}
bool DeviceManager::isWithinRestorePeriod(const std::string& deletedAtUtc, std::chrono::system_clock::time_point now) {
    const auto deletedAt = UtcTime::fromDbText(deletedAtUtc);
    if (!deletedAt) return true;
    return now < *deletedAt + std::chrono::hours(24 * kRetentionDays);
}

Result<RegisterOutcome> DeviceManager::registerCommissioned(ProtocolType type, const CommissionOutcome& outcome,
                                                            const std::string& name, const std::vector<DriverType>& driverTypes) {
    static const std::atomic<bool> kNotCancelled{false};
    return registerCommissioned(type, outcome, name, driverTypes, kNotCancelled);
}

Result<RegisterOutcome> DeviceManager::registerCommissioned(ProtocolType type, const CommissionOutcome& outcome,
                                                            const std::string& name, const std::vector<DriverType>& driverTypes,
                                                            const std::atomic<bool>& cancelRequested) {
    ProtocolDriver* driver = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto driverIt = drivers_.find(type);
        if (driverIt != drivers_.end()) driver = driverIt->second;
    }
    const std::string& id = outcome.deviceId;
    const std::string safeId = RequestValidation::sanitizeForLog(id);
    if (id.empty()) {
        std::cerr << "[DeviceManager] 커미셔닝 결과에 기기 ID가 없어 등록하지 않습니다" << std::endl;
        finalizeQuietly(driver, outcome, false);
        return Result<RegisterOutcome>::failure(ErrorCode::CommissionFailed, "empty device id");
    }

    // 같은 ID의 삭제(늦은 unpair 포함)가 끝난 뒤에 판정·저장·확정 훅·공개까지 한 번에 한다
    const auto idLock = registrationLockFor(id);
    std::lock_guard<std::mutex> registrationLock(*idLock);
    const auto decline = [&](ErrorCode code, const char* reason) {
        std::cerr << "[DeviceManager] 커미셔닝 결과를 등록하지 않음(" << reason << ") : " << safeId << std::endl;
        finalizeQuietly(driver, outcome, false);
        return Result<RegisterOutcome>::failure(code, reason);
    };

    // 락을 기다리는 사이 서버 종료·작업 취소가 들어왔으면 여기서 한 번에 거절한다(종료 중 DB에 새 기기를 쓰지 않음)
    if (isShuttingDown_ || cancelRequested) return decline(ErrorCode::Cancelled, "종료·취소");

    RegisterOutcome registered{id, false};
    std::optional<Device> saved;
    {
        // 복구 판정(findDevice)과 저장 사이에 정리 작업이 그 행을 지우면 "이력 없는 restored"가 된다. 정리와 겹치지 않게 한다
        std::lock_guard<std::mutex> retentionLock(retentionMutex_);
        const auto found = db_.findDevice(id);
        if (!found.ok()) return decline(ErrorCode::Internal, "기기 조회 실패");
        switch (found.value->kind) {
        case DeviceRecordState::Kind::Active:
            return decline(ErrorCode::AlreadyRegistered, "이미 등록된 기기");
        case DeviceRecordState::Kind::Hidden:
            if (isWithinRestorePeriod(found.value->deletedAtUtc, clock_.utcNow())) {
                registered.restored = true;  // insertDevice가 deleted_at을 지우고 통계·이력·ip를 이어 쓴다
            } else {
                // 정리 작업 전이라 남아 있는 지난 기록: 새 기기로 시작한다
                const Status purged = db_.purgeDeviceRecords(id);
                if (!purged.ok() && purged.error.code != ErrorCode::DeviceNotFound) {
                    return decline(ErrorCode::Internal, "지난 기록 삭제 실패");
                }
            }
            break;
        case DeviceRecordState::Kind::None:
            break;
        }
        saved = saveDevice(id, name, type, driverTypes, outcome.ip);
    }
    if (!saved) return decline(ErrorCode::Internal, "DB 저장 실패");
    // 드라이버 쪽 등록(Matter 매핑)을 확정한 뒤에 메모리에 공개한다. 공개 직후 들어온 제어·조회가 "매핑 없음"을 보지 않게
    finalizeQuietly(driver, outcome, true);
    publishDevice(std::move(*saved));
    std::cout << "[DeviceManager] 커미셔닝 등록 완료 : " << safeId << (registered.restored ? " (기록 복구)" : "") << std::endl;
    return Result<RegisterOutcome>::success(registered);
}

void DeviceManager::finalizeQuietly(ProtocolDriver* driver, const CommissionOutcome& outcome, bool accepted) {
    if (!driver) return;
    try {
        driver->finalizeCommission(outcome, accepted);
    } catch (const std::exception& e) {
        std::cerr << "[DeviceManager] 등록 확정 훅 예외 : " << RequestValidation::sanitizeForLog(outcome.deviceId)
                  << " (" << RequestValidation::sanitizeForLog(e.what()) << ")" << std::endl;
    } catch (...) {
        std::cerr << "[DeviceManager] 등록 확정 훅 알 수 없는 예외 : " << RequestValidation::sanitizeForLog(outcome.deviceId) << std::endl;
    }
}
void DeviceManager::onTcpConnected(const std::string& deviceId, const std::string& ip){
    bool writeDatabase = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(deviceId);
        if (it != devices_.end()) {
            if (it->second.protocol_type != ProtocolType::TCP_DIY || it->second.ip == ip) {
                writeDatabase = false;  // Matter 기기와 같은 ID이거나 바뀐 것이 없다
            } else {
                it->second.ip = ip;
            }
        }
    }
    // DB는 락 밖에서. 로드 전·미등록이면 0행으로 끝난다
    if (writeDatabase) db_.updateDeviceIp(deviceId, ip);
}
// 순서: [락] 맵에서 꺼냄 → DB에 숨김 표시(실패 시 되돌림) → 대기 명령 취소 → 락 없이 해제.
// 숨김을 먼저 저장해, 해제가 실패하거나 예외를 던져도 재시작 뒤 기기가 되살아나지 않게 한다.
Result<DeleteOutcome> DeviceManager::removeDevice(const std::string& id){
    const std::string safeId = RequestValidation::sanitizeForLog(id);
    // 없는 ID로 ID 락 항목을 만들지 않는다(없는 ID 반복 삭제로 맵이 커지지 않게). 아래에서 락 안에서 다시 확인한다
    if (!isRegistered(id)) {
        std::cerr << "[DeviceManager] 에러 : 기기를 찾을 수 없습니다 (" << safeId << ")" << std::endl;
        return Result<DeleteOutcome>::failure(ErrorCode::DeviceNotFound, "device not found");
    }
    // unpair까지 끝나야 같은 ID 등록이 시작된다(늦은 unpair가 새로 등록한 노드를 지우지 않게)
    const auto idLock = registrationLockFor(id);
    std::lock_guard<std::mutex> registrationLock(*idLock);
    Device removedDevice;
    ProtocolDriver* protocolDriver = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(id);
        if(it == devices_.end()){
            std::cerr << "[DeviceManager] 에러 : 기기를 찾을 수 없습니다 (" << safeId << ")" << std::endl;
            return Result<DeleteOutcome>::failure(ErrorCode::DeviceNotFound, "device not found");
        }
        auto driverIt = drivers_.find(it->second.protocol_type);
        if (driverIt != drivers_.end()) protocolDriver = driverIt->second;
        // 맵에서 먼저 빼서, 삭제가 진행되는 동안 들어온 요청은 "없는 기기"로 처리되게 한다
        removedDevice = std::move(it->second);
        devices_.erase(it);
    }
    DeleteOutcome outcome;
    outcome.deletedAtUtc = UtcTime::toDbText(clock_.utcNow());
    const auto marked = db_.markDeleted(id, outcome.deletedAtUtc);
    // DeviceNotFound = DB에는 이미 숨겨져 있음(같은 ID의 동시 추가·삭제 뒤 메모리에만 남은 경우).
    // 되돌리면 이 기기를 영영 지울 수 없으므로 삭제를 계속한다. 응답 시각은 기존 숨김 시각이 아니라 지금 시각
    if (!marked.ok() && marked.error.code != ErrorCode::DeviceNotFound) {
        std::cerr << "[DeviceManager] 에러 : DB 숨김 실패, 기기를 되돌립니다 (" << safeId << ")" << std::endl;
        std::lock_guard<std::mutex> lock(mutex_);
        // 그 사이 같은 ID가 다시 등록됐다면 새 기기를 덮어쓰지 않는다
        devices_.emplace(id, std::move(removedDevice));
        return Result<DeleteOutcome>::failure(ErrorCode::Internal, "mark deleted failed");
    }
    // 숨김이 확정된 뒤에 취소한다(DB 실패로 되돌릴 때 예약을 잃지 않게). 그 사이 만기된 명령은 isRegistered 확인이 막는다
    scheduler_.cancel(id, "삭제");
    outcome.unpaired = unpairQuietly(protocolDriver, id);
    std::cout << "[DeviceManager] 기기 삭제(숨김) 완료 : " << safeId << (outcome.unpaired ? "" : " (기기 쪽 해제 실패)") << std::endl;
    return Result<DeleteOutcome>::success(outcome);
}

// 드라이버 해제는 자기 락(소켓·파일)을 잡으므로 맵 락을 쥔 채 부르면 교착 위험이 있어 락 밖에서 부른다.
// 숨김은 이미 끝났으므로 실패·예외는 로그만 남기고 false로 알린다
bool DeviceManager::unpairQuietly(ProtocolDriver* protocolDriver, const std::string& id) {
    if (!protocolDriver) return false;
    try {
        if (protocolDriver->unpairDevice(id)) return true;
        std::cerr << "[DeviceManager] 경고 : 드라이버 해제 실패, 기기는 숨김 처리됨 (" << id << ")" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[DeviceManager] 경고 : 드라이버 해제 중 예외, 기기는 숨김 처리됨 (" << id << ": " << e.what() << ")" << std::endl;
    }
    return false;
}
// 없는 기기는 DeviceNotFound. 드라이버가 없거나 상태를 모르면 성공이되 "UNKNOWN"(저장된 상태는 바꾸지 않는다)
Result<std::string> DeviceManager::refreshState(const std::string& id, bool isManualRefresh) {
    ProtocolDriver* protocolDriver = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(id);
        if (it == devices_.end()) {
            return Result<std::string>::failure(ErrorCode::DeviceNotFound, "device not found");
        }
        auto driverIt = drivers_.find(it->second.protocol_type);
        if (driverIt != drivers_.end()) protocolDriver = driverIt->second;
    }
    if (!protocolDriver) return Result<std::string>::success("UNKNOWN");

    std::string realState = protocolDriver->readDeviceState(id, isManualRefresh); // 통신은 락 밖
    // 종료 중의 OFFLINE·UNKNOWN은 취소된 조회일 수 있다(Matter는 취소되면 UNKNOWN). 기록하지 않고 취소로 답한다
    if (isShuttingDown_ && (realState == "OFFLINE" || realState == "UNKNOWN")) {
        return Result<std::string>::failure(ErrorCode::Cancelled, "server shutting down");
    }
    if (realState != "UNKNOWN") updateDeviceState(id, realState);
    return Result<std::string>::success(std::move(realState));
}

// 기기가 없을 때도 "UNKNOWN"을 돌려주는 단순 버전(헬스체크용)
std::string DeviceManager::getDeviceState(std::string id, bool isManualRefresh) {
    const auto result = refreshState(id, isManualRefresh);
    return result.ok() ? *result.value : "UNKNOWN";
}
void DeviceManager::runHealthCheck() {
    std::cout << "\n[DeviceManager] 모든 기기의 상태 점검 시작..." << std::endl;

    const std::vector<Device> devices = getAllDevices();

    for (const auto& device : devices) {
        if (isShuttingDown_) break;
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
                if (energy.ok() && db_.insertEnergyLog(id, energy.value->totalEnergy_mWh, UtcTime::toDbText(clock_.utcNow()))) {
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
        // TCP 기기는 전력 측정 하드웨어가 없다. 드라이버 유무와 상관없이 같은 오류로 답한다
        if (!supportsEnergy(it->second.protocol_type)) {
            return Result<EnergySnapshot>::failure(ErrorCode::UnsupportedOperation, "TCP 기기는 전력 조회를 지원하지 않음");
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
        // 기기 존재는 위에서 확인했으므로 드라이버의 DeviceNotFound(매핑 없음)는 서버 내부 불일치다.
        // 그 밖의 코드는 드라이버가 정한 그대로 전달한다
        ErrorCode code = reading.error.code;
        if (code == ErrorCode::DeviceNotFound) code = ErrorCode::Internal;
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

bool DeviceManager::supportsEnergy(ProtocolType type) {
    return type == ProtocolType::MATTER; // 새 프로토콜은 전력 조회를 구현한 뒤에 추가한다
}

Result<HistoryResult> DeviceManager::getEnergyHistory(const std::string& deviceId, const HistoryQuery& query) {
    HistoryResult history;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(deviceId);
        if (it == devices_.end()) {
            return Result<HistoryResult>::failure(ErrorCode::DeviceNotFound, "device not found");
        }
        if (!supportsEnergy(it->second.protocol_type)) {
            return Result<HistoryResult>::failure(ErrorCode::UnsupportedOperation, "TCP 기기는 전력 이력을 지원하지 않음");
        }
        history.deviceState = it->second.state;
    }
    if (query.period) return aggregateHistory(deviceId, query, std::move(history));

    // 날짜 하나만 있어도 그쪽 경계만 적용한다
    std::optional<std::string> fromUtc;
    std::optional<std::string> toUtc;
    if (query.from) fromUtc = EnergyAggregation::localDateRangeToUtc(*query.from, *query.from).fromUtc;
    if (query.to) toUtc = EnergyAggregation::localDateRangeToUtc(*query.to, *query.to).toUtc;
    auto page = db_.queryHistory(deviceId, fromUtc, toUtc, static_cast<int>(std::min(query.limit, RequestValidation::kMaxHistoryLimit)));
    if (!page.ok()) return Result<HistoryResult>::failure(page.error.code, page.error.message);
    history.logs = std::move(page.value->logs);
    history.truncated = page.value->truncated;
    return Result<HistoryResult>::success(std::move(history));
}

Result<HistoryResult> DeviceManager::aggregateHistory(const std::string& deviceId, const HistoryQuery& query,
                                                      HistoryResult history) {
    if (!query.from || !query.to) {
        return Result<HistoryResult>::failure(ErrorCode::InvalidRequest, "집계에는 from·to 필요");
    }
    const UtcRange range = EnergyAggregation::localDateRangeToUtc(*query.from, *query.to);
    auto samples = db_.querySamplesForAggregation(deviceId, range.fromUtc, range.toUtc);
    if (!samples.ok()) return Result<HistoryResult>::failure(samples.error.code, samples.error.message);
    history.period = query.period;
    history.buckets = EnergyAggregation::aggregate(*samples.value, *query.from, *query.to, *query.period);
    return Result<HistoryResult>::success(std::move(history));
}

Result<PurgeResult> DeviceManager::runRetentionCleanup() {
    if (isShuttingDown_) return Result<PurgeResult>::failure(ErrorCode::Cancelled, "server shutting down");
    const auto cutoff = clock_.utcNow() - std::chrono::hours(24 * kRetentionDays);
    std::unique_lock<std::mutex> retentionLock(retentionMutex_);
    const auto result = db_.purgeExpired(UtcTime::toDbText(cutoff));
    retentionLock.unlock();
    if (!result.ok()) {
        std::cerr << "[DeviceManager] 정리 실패: " << result.error.message << std::endl;
    } else if (result.value->skipped) {
        std::cerr << "[DeviceManager] 경고 : 정리를 건너뜀 (" << result.value->reason << ")" << std::endl;
    } else {
        std::cout << "[DeviceManager] 정리: 이력 " << result.value->deletedLogs << "건, 기기 "
                  << result.value->deletedDevices << "개, 고아 " << result.value->deletedOrphans << "건" << std::endl;
    }
    return result;
}

std::shared_ptr<std::mutex> DeviceManager::energyLockFor(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(energyLocksMutex_);
    auto& slot = energyLocks_[deviceId];
    if (!slot) slot = std::make_shared<std::mutex>();
    return slot;
}

std::shared_ptr<std::mutex> DeviceManager::registrationLockFor(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(registrationLocksMutex_);
    auto& slot = registrationLocks_[deviceId];
    if (!slot) slot = std::make_shared<std::mutex>();
    return slot;
}
