#include <limits>
#include <algorithm>
#include "MatterController.hpp"
#include <optional>
#include "ChipToolOutputParser.hpp"
#include <iostream>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <ctime>
#include <memory>
#include <array>
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

MatterController::MatterController(ChipToolRunner& runner, SetupPinParser parseSetupPin,
                                   std::string chipToolPath, std::string configFilePath)
    : mChipToolPath(std::move(chipToolPath)),
      mRunner(runner),
      mParseSetupPin(std::move(parseSetupPin)),
      mConfigFilePath(std::move(configFilePath)) {
}

MatterController::~MatterController() {
    shutdown();
}

// 초기화
bool MatterController::Initialize(){
    std::vector<std::string> fileLines;
    {
        std::lock_guard<std::mutex> fileLock(fileMutex_);
        std::ifstream file(mConfigFilePath);
        std::string fileLine;
        while (std::getline(file, fileLine)) fileLines.push_back(fileLine);
    }
    int lineNumber = 0;

    for (const std::string& line : fileLines) {
        ++lineNumber;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue; // 빈 줄은 경고 없이 넘긴다.

        auto parsed = ChipToolOutputParser::parseRegisteredDeviceLine(line);
        if (!parsed.ok()) {
            // 줄 내용에 기기 정보가 있을 수 있어 번호만 남긴다.
            std::cerr << "[MatterController] 등록 파일 " << lineNumber << "번째 줄 형식 오류, 건너뜀" << std::endl;
            continue;
        }
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        mSerialToNodeId[parsed.value->first] = parsed.value->second;
    }
    std::cout << "[MatterController] 초기화 완료" << std::endl;
    return true;
}

// 종료
void MatterController::shutdown(){
    std::cout << "[MatterController] 종료" << std::endl;
}

std::string MatterController::runChipTool(const std::vector<std::string>& subcommand, std::chrono::milliseconds timeout,
                                          const std::vector<std::string>& secretsToMask) {
    // 셸 없이 인자 배열 그대로 실행한다. 출력 판정은 기존처럼 호출부가 문자열로 한다
    std::vector<std::string> args;
    args.reserve(subcommand.size() + 1);
    args.push_back(mChipToolPath);
    args.insert(args.end(), subcommand.begin(), subcommand.end());
    ProcessResult result = mRunner.run(args, "", timeout, secretsToMask);
    if (result.startFailed) std::cerr << "[MatterController] chip-tool 실행 실패" << std::endl;
    if (result.timedOut) std::cerr << "[MatterController] chip-tool 제한 시간 초과" << std::endl;
    return std::move(result.output);
}
std::string MatterController::runChipToolCommand(const std::vector<std::string>& subcommand, const std::string& deviceId){
    std::string output = runChipTool(subcommand, kCommandTimeout);
    bool isSuccess = ChipToolOutputParser::hasCommandSuccessMarker(output);
    if(output.empty() || !isSuccess){
        std::cerr << "[MatterController] 기기(" << deviceId << ") 통신 에러/오프라인. 사유: 출력 없음" << std::endl;
        markOffline(deviceId); // 오프라인 명부에 추가
        return "OFFLINE";
    }
    auto errorKeyword = ChipToolOutputParser::findCommandErrorKeyword(output);
    if (errorKeyword) {
        std::cerr << "[MatterController] 기기(" << deviceId << ") 통신 에러/오프라인. 사유: " << *errorKeyword << std::endl;
        markOffline(deviceId); // 오프라인 명부에 추가
        // 안드로이드가 쉽게 식별할 수 있도록 OFFLINE을 반환합니다.
        return "OFFLINE";
    }
    // 통신 성공 시 오프라인 명부에서 제거 (온라인 복구)
    if (clearOffline(deviceId)) {
        std::cout << "[MatterController] 기기(" << deviceId << ") 온라인 복구됨." << std::endl;
    }

    return output; 
}
bool MatterController::removeDeviceRegistration(const std::string& serialNumber) {
    // 조회와 삭제를 한 락 블록에서 한다. 따로 하면 그 사이 다른 스레드가 매핑을 바꿀 수 있다(TOCTOU)
    std::optional<uint64_t> foundNodeId;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        auto it = mSerialToNodeId.find(serialNumber);
        if (it != mSerialToNodeId.end()) {
            foundNodeId = it->second;
            mSerialToNodeId.erase(it);
            mPairingNodes.erase(*foundNodeId);
            mOfflineNodes.erase(std::to_string(*foundNodeId));
        }
    }

    // 텍스트 파일에서 해당 기기 번호만 삭제 (읽기~덮어쓰기를 한 락으로 묶어 다른 쓰기와 섞이지 않게 한다)
    {
        std::lock_guard<std::mutex> fileLock(fileMutex_);
        std::ifstream fileIn(mConfigFilePath);
        std::vector<std::string> lines;
        std::string line;

        if (fileIn.is_open()) {
            while (std::getline(fileIn, line)) {
                size_t commaPos = line.find(',');
                if (commaPos != std::string::npos) {
                    std::string savedSerial = line.substr(0, commaPos);
                    // 삭제하려는 시리얼 넘버와 다를 때만 백업
                    if (savedSerial != serialNumber) {
                        lines.push_back(line);
                    }
                }
            }
            fileIn.close();
        }

        // 백업해둔 줄들만 다시 파일에 덮어쓰기 (새로고침)
        std::ofstream fileOut(mConfigFilePath, std::ios::trunc);
        for (const auto& l : lines) {
            fileOut << l << "\n";
        }
    }

    if (!foundNodeId) {
        // 매핑이 없으면 노드 번호를 알 수 없다. 0번 노드에 unpair를 보내지 않도록 건너뛴다
        std::cerr << "[MatterController] 시리얼 매핑 없음, 페어링 해제 건너뜀: " << serialNumber << std::endl;
        return true; // 기존 응답 유지: 등록 정보 정리는 끝났다
    }
    const uint64_t deviceId = *foundNodeId;
    const std::string deviceIdStr = std::to_string(deviceId);

    // 2. chip-tool의 내부 KVS 데이터베이스에서도 이별 통보 (unpair)
    std::cout << "[MatterController] 기기(" << deviceIdStr << ") 페어링 해제 중..." << std::endl;
    std::thread([this,deviceId](){
        std::string deviceIdStr = std::to_string(deviceId);
        std::string output = this->runChipTool({"pairing", "unpair", deviceIdStr}, kSlowCommandTimeout);
        
        // 오프라인이어서 타임아웃이 나더라도 KVS에서는 지워지므로 상황에 맞게 로그만 분기
        if (output.find("Timeout") != std::string::npos || output.find("Error") != std::string::npos) {
            std::cerr << "[MatterController] 기기(" << deviceIdStr << ") 오프라인 상태. 로컬에서 강제 페어링 해제 완료." << std::endl;
        } else {
            std::cout << "[MatterController] 기기(" << deviceIdStr << ") 정상 페어링 해제 완료" << std::endl;
        }
    }).detach();
    return true;
}
bool MatterController::checkDeviceRegistered(uint64_t nodeId) {
    std::lock_guard<std::mutex> fileLock(fileMutex_);
    std::ifstream file(mConfigFilePath);
    std::string line;
    while (std::getline(file, line)) {
        size_t commaPos = line.find(',');
        if (commaPos != std::string::npos) {
            std::string savedNodeId = line.substr(commaPos + 1);
            if (savedNodeId == std::to_string(nodeId)) {
                return true; // 파일에 해당 Node ID가 존재하면 true
            }
        }
    }
    return false;
}

void MatterController::saveDeviceRegistration(const std::string& serialNumber, uint64_t nodeId) {
    std::lock_guard<std::mutex> fileLock(fileMutex_);
    std::ofstream file(mConfigFilePath, std::ios::app); // 파일 끝에 추가 모드
    file << serialNumber << "," << nodeId << "\n";
}
//기기 등록
Result<CommissionOutcome> MatterController::commissionDevice(uint64_t nodeId, const std::string& name,const std::string& manualPincode,
                                const std::string& wifiSsid, const std::string& wifiPassword,
                                const std::unordered_set<std::string>& activeDeviceIds){
    if (checkDeviceRegistered(nodeId)) {
        std::cout << "[MatterController]" << name <<  "(" << nodeId << ")는 이미 등록되어 있습니다. 커미셔닝을 건너뜁니다." << std::endl;
        std::string deviceIdStr = std::to_string(nodeId);

        if (activeDeviceIds.count(deviceIdStr) == 0) {
            std::cout << "[MatterController] ⚠️ DB에 기기 정보가 누락되어 복구(Sync)를 수행합니다." << std::endl;
            return Result<CommissionOutcome>::success(CommissionOutcome{deviceIdStr});
        }
        std::cout << "[MatterController] ✅ DB에도 이미 존재합니다. 커미셔닝을 완전히 건너뜁니다." << std::endl;
        return Result<CommissionOutcome>::success(CommissionOutcome{});
    }
    std::cout << "[MatterController] 기기 등록 시도: NodeId=" << nodeId << std::endl;
    const auto setupPin = mParseSetupPin ? mParseSetupPin(manualPincode)
                                         : Result<uint32_t>::failure(ErrorCode::Internal, "설정 코드 해석기 없음");
    if (!setupPin.ok()) {
        return Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "핀 코드 파싱 실패");
    }
    const std::string pinText = std::to_string(*setupPin.value);
   
    // SSID와 비밀번호는 REST 입력이므로 셸을 거치지 않는다.
    // str: 접두사는 hex:로 시작하는 값이 16진수로 디코딩되는 것을 막는다.
    const std::vector<std::string> pairingArgs = {
        "pairing", "ble-wifi", std::to_string(nodeId),
        "str:" + wifiSsid, "str:" + wifiPassword,
        pinText, "3830",
        "--paa-trust-store-path", "../paa_certs/paa-root-certs"
    };

    Result<CommissionOutcome> outcome =
        Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "chip-tool 페어링 실패");
    // 2. 에러 스트림 통합 및 결과 읽기. 출력의 비밀값은 러너가 ***로 가린다
    const std::string output = runChipTool(pairingArgs, kPairingTimeout,
                                           {wifiPassword, manualPincode, pinText});
    
    // 3. 성공 키워드 검사 (chip-tool 페어링 성공 로그 기준)
    if (ChipToolOutputParser::isCommissioningSuccessful(output)) {
        const std::string serialNumber = this->onDevicePairingComplete(nodeId, name);
        std::cout << "[MatterController] 커미셔닝 성공. 기기 정보를 내부 스토리지에 저장" << std::endl;
        outcome = Result<CommissionOutcome>::success(CommissionOutcome{serialNumber});
    } else {
        std::cerr << "[MatterController] 커미셔닝 실패 (타임아웃 또는 에러): \n" << output << std::endl;
    }

    return outcome;  // "페어링 중" 표시는 호출자의 PairingGuard가 푼다
}

std::string MatterController::getDeviceSerialNumber(uint64_t nodeId) {
    std::cout << "[MatterController] 기기 시리얼 넘버 조회 중... (NodeID: " << nodeId << ")" << std::endl;
    
    // chip-tool basic 클러스터 사용
    const std::string output = runChipTool({"basicinformation", "read", "serial-number", std::to_string(nodeId), "0"},
                                           kSlowCommandTimeout);

    auto serial = ChipToolOutputParser::extractSerialNumber(output);
    if (serial) return *serial;

    // 시리얼 넘버를 지원하지 않는 기기일 경우, 안전장치로 NodeId 기반의 문자열 반환
    return "UNKNOWN_SERIAL_" + std::to_string(nodeId);
}

// 기기 제어 Turn on
bool MatterController::turnOn(uint64_t nodeId,uint16_t endpointId){
    if (isPairing(nodeId)) {
        std::cout << "[MatterController] 기기 페어링 중... 켜기 요청 스킵." << std::endl;
        return false;
    }
    std::cout << "[MatterController] 💡 기기 켜기 요청 (NodeID: " << nodeId << ")" << std::endl;

    std::string output = runChipToolCommand({"onoff", "on", std::to_string(nodeId), std::to_string(endpointId)}, std::to_string(nodeId));
    if (output == "OFFLINE" || output == "UNKNOWN") {
        return false;
    }

    return true;
}

// 기기 제어 Turn off
bool MatterController::turnOff(uint64_t nodeId,uint16_t endpointId){
    if (isPairing(nodeId)) {
        std::cout << "[MatterController] ⏳ 기기 페어링 중. 끄기 요청 스킵." << std::endl;
        return false;
    }
    std::cout << "[MatterController] 🔌 기기 끄기 요청 (NodeID: " << nodeId << ")" << std::endl;

    std::string output = runChipToolCommand({"onoff", "off", std::to_string(nodeId), std::to_string(endpointId)}, std::to_string(nodeId));
    if (output == "OFFLINE" || output == "UNKNOWN") {
        return false;
    }

    return true;
}

// 인터페이스 구현
bool MatterController::sendCommand(const std::string& deviceId, const std::string& command){
    
    uint64_t nodeId = getNodeId(deviceId);

    if(command == "ON"|| command == "TURN_ON"){
        return turnOn(nodeId,1);
    } else if(command == "OFF"|| command == "TURN_OFF"){
        return turnOff(nodeId,1);
    }
    return false;
}
Result<CommissionOutcome> MatterController::commission(const CommissionRequest& request,
                                                       const std::unordered_set<std::string>& activeDeviceIds){
    const uint64_t nodeId = reserveNodeIdForPairing();
    PairingGuard pairingGuard(*this, nodeId);
    return commissionDevice(nodeId, request.name, request.payload, request.ssid, request.password, activeDeviceIds);
}
std::string MatterController::onDevicePairingComplete(uint64_t nodeId, const std::string& deviceName) {
    std::cout << "[MatterController] 기기 페어링 완료: NodeId=" << nodeId << ", DeviceName=" << deviceName << std::endl;
    std::string serialNumber = getDeviceSerialNumber(nodeId);
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        mSerialToNodeId[serialNumber] = nodeId; // 시리얼 넘버와 NodeId 매핑 저장
    }
    
    saveDeviceRegistration(serialNumber, nodeId);
    return serialNumber;
}
bool MatterController::unpairDevice(const std::string& deviceId){
    std::cout << "[MatterController] Matter 기기 페어링 해제 : NodeId = " << deviceId << std::endl;
    bool success = this->removeDeviceRegistration(deviceId);
    return success;
}
std::string MatterController::readDeviceState(const std::string& rawDeviceId, bool isManualRequest){
    std::string deviceId = rawDeviceId;
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), '\n'), deviceId.end());
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), '\r'), deviceId.end());
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), '\"'), deviceId.end());
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), ' '), deviceId.end());

    uint64_t nodeId = getNodeId(deviceId);

    // 기기가 현재 페어링 중이면 즉시 스킵
    if (isPairing(nodeId)) {
        std::cout << "[MatterController] 기기(" << nodeId << ") 페어링 중. 헬스체크 스킵." << std::endl;
        return "PAIRING";
    }

    // 자동 헬스체크인데 기기가 오프라인 명부에 있다면 쉘 명령 실행 스킵
    if (!isManualRequest && isOffline(std::to_string(nodeId))) {
        std::cout << "[MatterController] 기기(" << nodeId << ") 오프라인 상태. 헬스체크 스킵." << std::endl;
        return "OFFLINE";
    }

    std::cout << "[MatterController] 기기 상태 읽기 요청 : NodeId = " << nodeId << std::endl;

    try{
        std::string output = this->runChipToolCommand({"onoff", "read", "on-off", std::to_string(nodeId), "1"}, std::to_string(nodeId));
        
        if (output == "OFFLINE" || output == "UNKNOWN") return output;

        auto onOffState = ChipToolOutputParser::parseOnOffState(output);
        if (onOffState) return *onOffState;
    }catch(const std::exception& e){
        std::cerr << "[MatterController] 상태 읽기 실패: " << e.what() << std::endl;
        return "ERROR";
    }
    return "UNKNOWN";
}
// 값 해석 규칙은 ChipToolOutputParser::parseEnergyReading에 있다
Result<EnergyReading> MatterController::readEnergy(const std::string& deviceId){
    // 매핑이 없으면 노드 0에 chip-tool을 실행하게 되므로 바로 실패한다
    if (getNodeId(deviceId) == 0) {
        return Result<EnergyReading>::failure(ErrorCode::DeviceNotFound, "시리얼 매핑 없음");
    }
    const std::string activePowerText = getPowerUsage(deviceId);
    // 첫 조회에서 닿지 못했으면 두 번째 chip-tool 실행(최대 5초)을 생략한다
    if (ChipToolOutputParser::isUnreachableMarker(activePowerText)) {
        return ChipToolOutputParser::parseEnergyReading(activePowerText, "");
    }
    const std::string cumulativeText = getCumulativeEnergy(deviceId);
    return ChipToolOutputParser::parseEnergyReading(activePowerText, cumulativeText);
}
std::string MatterController::getPowerUsage(std::string deviceId, bool isManualRequest){
    uint64_t nodeId = getNodeId(deviceId);

    // 기기가 현재 페어링 중이면 스킵
    if (isPairing(nodeId)) return "PAIRING";

    // 오프라인 기기의 자동 조회 스킵
    if (!isManualRequest && isOffline(std::to_string(nodeId))) return "OFFLINE";

    std::cout << "[MatterController] 기기 전력량(실시간) 조회 요청 : NodeId = " << nodeId << std::endl;
    std::string output = this->runChipToolCommand(
        {"electricalpowermeasurement", "read", "active-power", std::to_string(nodeId), "1"}, std::to_string(nodeId));

    if (output == "OFFLINE" || output == "UNKNOWN") return output;

    if (output.empty()) return "UNKNOWN";

    std::string result = ChipToolOutputParser::extractActivePowerText(output).value_or("UNKNOWN");

    if (result != "UNKNOWN") {
        std::cout << "[MatterController] 실시간 전력 : " << result << " mW" << std::endl;
    }
    return result;
}
std::string MatterController::getCumulativeEnergy(std::string deviceId, bool isManualRequest){
    const uint64_t nodeIdValue = getNodeId(deviceId);
    std::string nodeId = std::to_string(nodeIdValue);

    // getPowerUsage와 같은 건너뛰기 규칙
    if (isPairing(nodeIdValue)) return "PAIRING";
    if (!isManualRequest && isOffline(nodeId)) return "OFFLINE";

    std::cout << "[MatterController] 기기 전력량(누적) 조회 요청 : NodeId = " << nodeId << std::endl;
    // 러너가 stderr도 함께 읽으므로(셸의 2>&1과 같음) 오류가 stderr로만 나와도 판정에 쓰인다
    std::string output = this->runChipToolCommand(
        {"electricalenergymeasurement", "read", "cumulative-energy-imported", nodeId, "1"}, nodeId);

    if (output == "OFFLINE" || output == "UNKNOWN") return output;

    if (output.empty()) return "UNKNOWN";

    std::string result = ChipToolOutputParser::extractEnergyText(output).value_or("UNKNOWN");
    if (result != "UNKNOWN") {
        std::cout << "[MatterController] 누적 전력 : " << result << " mWh" << std::endl;
    }
    return result;
}
std::uint64_t MatterController::getNodeId(const std::string& serialNumber) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    auto it = mSerialToNodeId.find(serialNumber);
    if (it != mSerialToNodeId.end()) {
        return it->second;
    }
    return 0;
}

// ---- 상태 명부 접근: 모두 stateMutex_ 안에서 메모리만 다룬다 ----
bool MatterController::isPairing(uint64_t nodeId) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    return mPairingNodes.count(nodeId) > 0;
}
void MatterController::setPairing(uint64_t nodeId, bool pairing) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    if (pairing) mPairingNodes.insert(nodeId);
    else mPairingNodes.erase(nodeId);
}
bool MatterController::isOffline(const std::string& nodeIdText) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    return mOfflineNodes.count(nodeIdText) > 0;
}
void MatterController::markOffline(const std::string& nodeIdText) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    mOfflineNodes[nodeIdText] = std::chrono::steady_clock::now();
}
bool MatterController::clearOffline(const std::string& nodeIdText) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    return mOfflineNodes.erase(nodeIdText) > 0;
}

uint64_t MatterController::reserveNodeIdForPairing() {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    uint64_t candidate = std::max<uint64_t>(static_cast<uint64_t>(std::time(nullptr)), mLastIssuedNodeId + 1);
    auto inUse = [this](uint64_t id) {
        if (mPairingNodes.count(id) > 0) return true;
        for (const auto& pair : mSerialToNodeId) if (pair.second == id) return true;
        return false;
    };
    while (inUse(candidate)) ++candidate;
    mLastIssuedNodeId = candidate;
    mPairingNodes.insert(candidate);
    return candidate;
}
