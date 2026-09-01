#include "MatterController.hpp"
#include "DeviceManager.hpp"
#include <iostream>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <ctime>
#include <memory>
#include <array>
MatterController::MatterController(DeviceManager& deviceManager)
    : ProtocolDriver(deviceManager),
      mChipToolPath("../third_party/connectedhomeip/out/host/chip-tool"),
      mConfigFilePath("../config/registered_devices.txt") {
}

MatterController::~MatterController() {
    shutdown();
}

// 초기화
bool MatterController::Initialize(){
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
std::string MatterController::executeCommandWithErrorResponse(const std::string& cmd, const std::string& deviceId){
    std::string output = executeCommandWithOutput(cmd);
        std::vector<std::string> errorKeywords = {
            "Timeout",
            "Error resolving node",
            "AccessDenied",
            "Incorrect state",
            "CHIP_ERROR_"
        };

        for (const auto& keyword : errorKeywords) {
            if (output.find(keyword) != std::string::npos) {
                std::cerr << "[MatterController] 기기(" << deviceId << ") 통신 에러/오프라인. 사유: " << keyword << std::endl;
                mOfflineNodes[deviceId] = std::chrono::steady_clock::now(); // 오프라인 명부에 추가
                // 안드로이드가 쉽게 식별할 수 있도록 OFFLINE을 반환합니다.
                return "OFFLINE"; 
            }
        }
        // 통신 성공 시 오프라인 명부에서 제거 (온라인 복구)
        if (mOfflineNodes.count(deviceId) > 0) {
            mOfflineNodes.erase(deviceId);
        }

    return output; 
}
bool MatterController::removeDeviceRegistration(uint64_t deviceId) {
    std::string deviceIdStr = std::to_string(deviceId);

    // 방어막(캐시) 명부에서 기기 정보 즉시 완전 삭제
    mPairingNodes.erase(deviceId);
    mOfflineNodes.erase(deviceIdStr);

    // 텍스트 파일에서 해당 기기 번호만 삭제
    std::ifstream fileIn(mConfigFilePath);
    std::vector<std::string> lines;
    std::string line;
    
    if (fileIn.is_open()) {
        while (std::getline(fileIn, line)) {
            // 삭제하려는 nodeId가 아닌 줄만 백업해 둠
            if (line != std::to_string(deviceId)) {
                lines.push_back(line);
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
    std::ifstream file(mConfigFilePath);
    std::string line;
    while (std::getline(file, line)) {
        if (line == std::to_string(nodeId)) {
            return true; // 파일에 Node ID가 존재하면 true
        }
    }
    return false;
}

void MatterController::saveDeviceRegistration(uint64_t nodeId) {
    std::ofstream file(mConfigFilePath, std::ios::app); // 파일 끝에 추가 모드
    file << nodeId << "\n";
}
//기기 등록
bool MatterController::commissionDevice(uint64_t nodeId, std::string name,const std::string& manualPincode,
                                const std::string& wifiSsid, const std::string& wifiPassword){
    if (checkDeviceRegistered(nodeId)) {
        std::cout << "[MatterController]" << name <<  "(" << nodeId << ")는 이미 등록되어 있습니다. 커미셔닝을 건너뜁니다." << std::endl;
        std::string deviceIdStr = std::to_string(nodeId);

        if (!mDeviceManager.hasDevice(deviceIdStr)) {
            std::cout << "[MatterController] ⚠️ DB에 기기 정보가 누락되어 복구(Sync)를 수행합니다." << std::endl;
            
            mDeviceManager.addDevice(deviceIdStr, name, ProtocolType::MATTER);
        } else {
            std::cout << "[MatterController] ✅ DB에도 이미 존재합니다. 커미셔닝을 완전히 건너뜁니다." << std::endl;
        }
        return true; 
    }
    std::cout << "[MatterController] 기기 등록 시도: NodeId=" << nodeId << ", PinCode=" << manualPincode << std::endl;
    chip::SetupPayload payload;
    CHIP_ERROR err = chip::ManualSetupPayloadParser(manualPincode).populatePayload(payload);
    if (err != CHIP_NO_ERROR) {
        std::cerr << "[MatterController] ❌ 핀 코드 파싱 실패. Error: " << err.Format() << std::endl;
        return false;
    }
   
    std::string cmd = mChipToolPath + " pairing ble-wifi " + std::to_string(nodeId) + " " + wifiSsid + " " + wifiPassword + " " 
            + std::to_string(payload.setUpPINCode) + " " + "3830"+ 
            " --paa-trust-store-path ../paa_certs/paa-root-certs";

    mPairingNodes.insert(nodeId);

    bool isSuccess = false;
    // 2. 에러 스트림 통합 및 결과 읽기 (executeCommand 대신 사용)
    std::string cmdWithErr = cmd + " 2>&1";
    std::string output = executeCommandWithOutput(cmdWithErr);
    
    // 3. 성공 키워드 검사 (chip-tool 페어링 성공 로그 기준)
    if (output.find("Device commissioning completed with success") != std::string::npos ||
        output.find("CHIP_NO_ERROR") != std::string::npos) {
        this->onDevicePairingComplete(nodeId, name);
        std::cout << "[MatterController] 커미셔닝 성공. 기기 정보를 내부 스토리지에 저장" << std::endl;
        isSuccess = true;
    } else {
        std::cerr << "[MatterController] 커미셔닝 실패 (타임아웃 또는 에러): \n" << output << std::endl;
        isSuccess = false;
    }

    // 4. 작업 완료 후 Lock 해제
    mPairingNodes.erase(nodeId);
    
    return isSuccess;
}

// 기기 제어 Turn on
bool MatterController::turnOn(uint64_t nodeId,uint16_t endpointId){
    if (mPairingNodes.count(nodeId) > 0) {
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
    if (mPairingNodes.count(nodeId) > 0) {
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
bool MatterController::sendCommand(std::string deviceId, std::string command){
    uint64_t nodeId = std::stoull(deviceId);
    if(command == "ON"|| command == "TURN_ON"){
        return turnOn(nodeId,1);
    } else if(command == "OFF"|| command == "TURN_OFF"){
        return turnOff(nodeId,1);
    }
    return false;
}
bool MatterController::commissionDevice(std::string name, std::string payload){
    uint64_t nodeId = static_cast<uint64_t>(std::time(nullptr));
    return commissionDevice(nodeId,name,payload,"U+Net8683","38835318M#");
}
void MatterController::onDevicePairingComplete(uint64_t nodeId, const std::string& deviceName) {
    std::cout << "[MatterController] 기기 페어링 완료: NodeId=" << nodeId << ", DeviceName=" << deviceName << std::endl;
    saveDeviceRegistration(nodeId);
    std::string newId = std::to_string(nodeId);
    mDeviceManager.addDevice(newId, deviceName, ProtocolType::MATTER);
}
bool MatterController::unpairDevice(std::string deviceId){
    std::cout << "[MatterController] Matter 기기 페어링 해제 : NodeId = " << deviceId << std::endl;
    bool success = this->removeDeviceRegistration(std::stoull(deviceId));
    return success;
}
std::string MatterController::readDeviceState(std::string deviceId, bool isManualRequest){
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), '\n'), deviceId.end());
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), '\r'), deviceId.end());
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), '\"'), deviceId.end());
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), ' '), deviceId.end());

    uint64_t nodeId = std::stoull(deviceId);

    // 기기가 현재 페어링 중이면 즉시 스킵
    if (mPairingNodes.count(nodeId) > 0) {
        std::cout << "[MatterController] 기기(" << deviceId << ") 페어링 중. 헬스체크 스킵." << std::endl;
        return "PAIRING";
    }

    // 자동 헬스체크인데 기기가 오프라인 명부에 있다면 쉘 명령 실행 스킵
    if (!isManualRequest && mOfflineNodes.count(deviceId) > 0) {
        std::cout << "[MatterController] 기기(" << deviceId << ") 오프라인 상태. 헬스체크 스킵." << std::endl;
        return "OFFLINE";
    }

    std::cout << "[MatterController] 기기 상태 읽기 요청 : NodeId = " << deviceId << std::endl;
    std::string cmd = mChipToolPath + " onoff read on-off " + deviceId + " 1 2>&1";
    try{
        std::string output = this->executeCommandWithErrorResponse(cmd, deviceId);
        
        if (output == "OFFLINE" || output == "UNKNOWN") return output;

        // 출력된 로그 중에 Data = true(또는 1)이 있으면 ON
        if (output.find("Data = true") != std::string::npos || output.find("Data: 1") != std::string::npos) {
            return "ON";
        } 
        // 출력된 로그 중에 Data = false(또는 0)이 있으면 OFF
        else if (output.find("Data = false") != std::string::npos || output.find("Data: 0") != std::string::npos) {
            return "OFF";
        }
    }catch(const std::exception& e){
        std::cerr << "[MatterController] 상태 읽기 실패: " << e.what() << std::endl;
        return "ERROR";
    }
    return "UNKNOWN";
}
std::string MatterController::getPowerUsage(std::string deviceId, bool isManualRequest){
    uint64_t nodeId = std::stoull(deviceId);

    // 기기가 현재 페어링 중이면 스킵
    if (mPairingNodes.count(nodeId) > 0) return "PAIRING";

    // 오프라인 기기의 자동 조회 스킵
    if (!isManualRequest && mOfflineNodes.count(deviceId) > 0) return "OFFLINE";

    std::cout << "[MatterController] 기기 전력량(실시간) 조회 요청 : NodeId = " << deviceId << std::endl;
    std::string cmd = mChipToolPath + " electricalpowermeasurement read active-power " + deviceId + " 1 2>&1";
    
    std::string output = this->executeCommandWithErrorResponse(cmd, deviceId);

    if (output == "OFFLINE" || output == "UNKNOWN") return output;

    if (output.empty()) return "UNKNOWN";

    std::string result = "UNKNOWN";
    std::istringstream iss(output);
    std::string line;

    while (std::getline(iss, line)) {
        size_t pos = line.find("ActivePower:");
        if (pos != std::string::npos) {
            std::string valStr = line.substr(pos + 12);
            // 앞뒤 공백 청소
            valStr.erase(0, valStr.find_first_not_of(" \t"));
            valStr.erase(valStr.find_last_not_of(" \n\r\t") + 1);
            result = valStr;
            break;
        }
    }

    if (result != "UNKNOWN") {
        std::cout << "[MatterController] 실시간 전력 : " << result << " mW" << std::endl;
    }
    return result;
}
std::string MatterController::getCumulativeEnergy(std::string deviceId, bool isManualRequest){
    std::cout << "[MatterController] 기기 전력량(누적) 조회 요청 : NodeId = " << deviceId << std::endl;
    std::string cmd = mChipToolPath + " electricalenergymeasurement read cumulative-energy-imported " + deviceId + " 1";
    
    std::string output = this->executeCommandWithErrorResponse(cmd, deviceId);

    if (output == "OFFLINE" || output == "UNKNOWN") return output;

    if (output.empty()) return "UNKNOWN";

    std::string result = "UNKNOWN";
    std::istringstream iss(output);
    std::string line;
    
    while (std::getline(iss, line)) {
        size_t pos = line.find("Energy:");
        if (pos != std::string::npos) {
            std::string valStr = line.substr(pos + 7);
            // 앞뒤 공백 청소
            valStr.erase(0, valStr.find_first_not_of(" \t"));
            valStr.erase(valStr.find_last_not_of(" \n\r\t") + 1);
            result = valStr;
            break;
        }
    }
    if (result != "UNKNOWN") {
        std::cout << "[MatterController] 누적 전력 : " << result << " mWh" << std::endl;
    }
    return result;
}