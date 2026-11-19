#include <limits>
#include "MatterController.hpp"
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

namespace {
    // 외부 프로그램 출력은 내용을 보장할 수 없으므로, 로그에 남기기 전에 비밀값을 가린다.
    std::string redactSecrets(std::string text, const std::vector<std::string>& secrets) {
        const std::string mask = "***";
        for (const auto& secret : secrets) {
            if (secret.empty()) continue; // 빈 문자열은 무한 반복을 일으키므로 건너뛴다.
            size_t position = 0;
            while ((position = text.find(secret, position)) != std::string::npos) {
                text.replace(position, secret.size(), mask);
                position += mask.size();
            }
        }
        return text;
    }
}

MatterController::MatterController()
    : mChipToolPath("../third_party/connectedhomeip/out/host/chip-tool"),
      mConfigFilePath("../config/registered_devices.txt") {
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

bool MatterController::executeCommand(const std::string& cmd) {
    std::cout << "[MatterController]  쉘 명령 실행: " << cmd << std::endl;
    
    // std::system은 명령이 성공적으로 끝나면 0을 반환합니다.
    int result = std::system(cmd.c_str());
    
    if (result == 0) {
        std::cout << "[MatterController] 명령 전송 성공!" << std::endl;
        return true;
    } else {
        std::cerr << "[MatterController] 명령 실패. (리턴 코드: " << result << ")" << std::endl;
        return false;
    }
}
std::string MatterController::executeCommandWithOutput(std::string cmd){
    std::array<char,128> buffer;
    std::string result;
    std::unique_ptr<FILE,int(*)(FILE*)> pipe(popen(cmd.c_str(),"r"),pclose);
    if(!pipe){
        std::cerr << "[MatterController] 명령 실행 실패: " << cmd << std::endl;
        return "";
    }
    while(fgets(buffer.data(),buffer.size(),pipe.get())!=nullptr){
        result += buffer.data();
    }
    return result;
}
std::string MatterController::executeArgsWithOutput(const std::vector<std::string>& args){
    // 멀티스레드 프로세스에서 fork한 자식은 malloc을 쓰면 교착될 수 있으므로, argv를 fork 전에 준비한다.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC) != 0) {
        std::cerr << "[MatterController] 파이프 생성 실패" << std::endl;
        return "";
    }

    pid_t childPid = fork();
    if (childPid < 0) {
        std::cerr << "[MatterController] 프로세스 생성 실패" << std::endl;
        close(pipeFds[0]);
        close(pipeFds[1]);
        return "";
    }
    if (childPid == 0) {
        // 자식: 기존 "2>&1"과 같게 stdout과 stderr를 파이프로 보낸다.
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        execv(argv[0], argv.data());
        _exit(127); // exec 실패. 부모의 버퍼를 비우지 않도록 _exit를 쓴다.
    }

    close(pipeFds[1]);
    std::string output;
    std::array<char, 128> buffer;
    while (true) {
        ssize_t readBytes = read(pipeFds[0], buffer.data(), buffer.size());
        if (readBytes > 0) {
            output.append(buffer.data(), static_cast<size_t>(readBytes));
        } else if (readBytes == 0 || errno != EINTR) {
            break;
        }
    }
    close(pipeFds[0]);

    int childStatus = 0;
    while (waitpid(childPid, &childStatus, 0) < 0 && errno == EINTR) {} // 좀비 프로세스 방지
    return output; // 성공 여부는 기존처럼 호출부에서 출력 문자열로 판단한다.
}
std::string MatterController::executeCommandWithErrorResponse(const std::string& cmd, const std::string& deviceId){
    std::string output = executeCommandWithOutput("timeout 5 " + cmd);
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
    uint64_t deviceId = getNodeId(serialNumber);
    std::string deviceIdStr = std::to_string(deviceId);

    // 방어막(캐시) 명부와 시리얼 매핑에서 기기 정보 즉시 완전 삭제
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        mPairingNodes.erase(deviceId);
        mOfflineNodes.erase(deviceIdStr);
        mSerialToNodeId.erase(serialNumber);
    }

    // 텍스트 파일에서 해당 기기 번호만 삭제 (읽기~덮어쓰기를 한 락으로 묶어 다른 쓰기와 섞이지 않게 한다)
    std::unique_lock<std::mutex> fileLock(fileMutex_);
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
    fileOut.close();
    fileLock.unlock();

    // 2. chip-tool의 내부 KVS 데이터베이스에서도 이별 통보 (unpair)
    std::string cmd = mChipToolPath + " pairing unpair " + std::to_string(deviceId) + " 2>&1";
    std::cout << "[MatterController] 기기(" << deviceIdStr << ") 페어링 해제 중..." << std::endl;
    std::thread([this,cmd,deviceId](){
        std::string deviceIdStr = std::to_string(deviceId);
        std::string output = this->executeCommandWithOutput(cmd);
        
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
    chip::SetupPayload payload;
    CHIP_ERROR err = chip::ManualSetupPayloadParser(manualPincode).populatePayload(payload);
    if (err != CHIP_NO_ERROR) {
        std::cerr << "[MatterController] ❌ 핀 코드 파싱 실패. Error: " << err.Format() << std::endl;
        return Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "핀 코드 파싱 실패");
    }
   
    // SSID와 비밀번호는 REST 입력이므로 셸을 거치지 않는다.
    // str: 접두사는 hex:로 시작하는 값이 16진수로 디코딩되는 것을 막는다.
    const std::vector<std::string> pairingArgs = {
        mChipToolPath, "pairing", "ble-wifi", std::to_string(nodeId),
        "str:" + wifiSsid, "str:" + wifiPassword,
        std::to_string(payload.setUpPINCode), "3830",
        "--paa-trust-store-path", "../paa_certs/paa-root-certs"
    };

    setPairing(nodeId, true);

    Result<CommissionOutcome> outcome =
        Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "chip-tool 페어링 실패");
    // 2. 에러 스트림 통합 및 결과 읽기 (executeCommand 대신 사용)
    std::string output = executeArgsWithOutput(pairingArgs);
    
    // 3. 성공 키워드 검사 (chip-tool 페어링 성공 로그 기준)
    if (ChipToolOutputParser::isCommissioningSuccessful(output)) {
        const std::string serialNumber = this->onDevicePairingComplete(nodeId, name);
        std::cout << "[MatterController] 커미셔닝 성공. 기기 정보를 내부 스토리지에 저장" << std::endl;
        outcome = Result<CommissionOutcome>::success(CommissionOutcome{serialNumber});
    } else {
        const std::string safeOutput = redactSecrets(output, {
            wifiPassword, manualPincode, std::to_string(payload.setUpPINCode)
        });
        std::cerr << "[MatterController] 커미셔닝 실패 (타임아웃 또는 에러): \n" << safeOutput << std::endl;
    }

    // 4. 작업 완료 후 Lock 해제
    setPairing(nodeId, false);

    return outcome;
}

std::string MatterController::getDeviceSerialNumber(uint64_t nodeId) {
    std::cout << "[MatterController] 기기 시리얼 넘버 조회 중... (NodeID: " << nodeId << ")" << std::endl;
    
    // chip-tool basic 클러스터 사용
    std::string cmd = mChipToolPath + " basicinformation read serial-number " + std::to_string(nodeId) + " 0 2>&1";
    std::string output = executeCommandWithOutput(cmd);

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

    std::string cmd = mChipToolPath + " onoff on " + std::to_string(nodeId) + " " + std::to_string(endpointId) + " 2>&1";
    std::string output = executeCommandWithErrorResponse(cmd, std::to_string(nodeId));
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

    std::string cmd = mChipToolPath + " onoff off " + std::to_string(nodeId) + " " + std::to_string(endpointId) + " 2>&1";
    std::string output = executeCommandWithErrorResponse(cmd, std::to_string(nodeId));
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
    uint64_t nodeId = static_cast<uint64_t>(std::time(nullptr));
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
    std::string cmd = mChipToolPath + " onoff read on-off " + std::to_string(nodeId) + " 1 2>&1";
    try{
        std::string output = this->executeCommandWithErrorResponse(cmd, std::to_string(nodeId));
        
        if (output == "OFFLINE" || output == "UNKNOWN") return output;

        auto onOffState = ChipToolOutputParser::parseOnOffState(output);
        if (onOffState) return *onOffState;
    }catch(const std::exception& e){
        std::cerr << "[MatterController] 상태 읽기 실패: " << e.what() << std::endl;
        return "ERROR";
    }
    return "UNKNOWN";
}
// 값 해석 규칙: "UNKNOWN"/빈 문자열은 0, 그 외 숫자가 아니면 실패
Result<EnergyReading> MatterController::readEnergy(const std::string& deviceId){
    const std::string activePowerText = getPowerUsage(deviceId);
    const std::string cumulativeText = getCumulativeEnergy(deviceId);

    EnergyReading reading{0, 0};
    if (activePowerText != "UNKNOWN" && !activePowerText.empty()) {
        const auto parsedPower = ChipToolOutputParser::parseInteger(activePowerText);
        if (!parsedPower.ok() ||
            *parsedPower.value < std::numeric_limits<int>::min() ||
            *parsedPower.value > std::numeric_limits<int>::max()) {
            return Result<EnergyReading>::failure(ErrorCode::Internal, "실시간 전력 값 해석 실패");
        }
        reading.activePower_mW = *parsedPower.value;
    }
    if (cumulativeText != "UNKNOWN" && !cumulativeText.empty()) {
        const auto parsedEnergy = ChipToolOutputParser::parseInteger(cumulativeText);
        if (!parsedEnergy.ok()) {
            return Result<EnergyReading>::failure(ErrorCode::Internal, "누적 전력 값 해석 실패");
        }
        reading.cumulative_mWh = *parsedEnergy.value;
    }
    return Result<EnergyReading>::success(reading);
}
std::string MatterController::getPowerUsage(std::string deviceId, bool isManualRequest){
    uint64_t nodeId = getNodeId(deviceId);

    // 기기가 현재 페어링 중이면 스킵
    if (isPairing(nodeId)) return "PAIRING";

    // 오프라인 기기의 자동 조회 스킵
    if (!isManualRequest && isOffline(std::to_string(nodeId))) return "OFFLINE";

    std::cout << "[MatterController] 기기 전력량(실시간) 조회 요청 : NodeId = " << nodeId << std::endl;
    std::string cmd = mChipToolPath + " electricalpowermeasurement read active-power " + std::to_string(nodeId) + " 1 2>&1";
    
    std::string output = this->executeCommandWithErrorResponse(cmd, std::to_string(nodeId));

    if (output == "OFFLINE" || output == "UNKNOWN") return output;

    if (output.empty()) return "UNKNOWN";

    std::string result = ChipToolOutputParser::extractActivePowerText(output).value_or("UNKNOWN");

    if (result != "UNKNOWN") {
        std::cout << "[MatterController] 실시간 전력 : " << result << " mW" << std::endl;
    }
    return result;
}
std::string MatterController::getCumulativeEnergy(std::string deviceId, bool isManualRequest){
    std::string nodeId = std::to_string(getNodeId(deviceId));
    std::cout << "[MatterController] 기기 전력량(누적) 조회 요청 : NodeId = " << nodeId << std::endl;
    std::string cmd = mChipToolPath + " electricalenergymeasurement read cumulative-energy-imported " + nodeId + " 1";
    
    std::string output = this->executeCommandWithErrorResponse(cmd, nodeId);

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
