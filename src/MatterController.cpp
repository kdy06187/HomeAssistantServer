#include "MatterController.hpp"
#include "DeviceManager.hpp"
#include <iostream>
#include <cstdlib>
#include <fstream>
#include <ctime>
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

bool MatterController::removeDeviceRegistration(uint64_t deviceId) {
    // 1. 텍스트 파일에서 해당 기기 번호만 삭제
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
    std::string cmd = mChipToolPath + " pairing unpair " + std::to_string(deviceId);
    std::cout << "[MatterController] 페어링 해제 중..." << std::endl;
    std::thread([this,cmd,deviceId](){
        bool success = this->executeCommand(cmd);
        if (success) {
            std::cout << "[MatterController] 기기(" << deviceId << ") 페어링 해제 완료" << std::endl;
        } else {
            std::cerr << "[MatterController] 기기(" << deviceId << ") 페어링 해제 실패" << std::endl;
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
    std::thread([this, cmd, nodeId, name]() {
        bool success = executeCommand(cmd);
        if (success) {
            this->onDevicePairingComplete(nodeId, name);
            std::cout << "[MatterController] 커미셔닝 성공. 기기 정보를 내부 스토리지에 저장" << std::endl;
        }
    }).detach();
    return true;
}

// 기기 제어 Turn on
bool MatterController::turnOn(uint64_t nodeId,uint16_t endpointId){
    std::cout << "[MatterController] 💡 기기 켜기 요청 전송 중..." << std::endl;
    std::cout << " -> Target NodeId: " << nodeId << ", EndpointId: " << endpointId << std::endl;

    std::string cmd = mChipToolPath + " onoff on " + std::to_string(nodeId) + " " + std::to_string(endpointId);
    return executeCommand(cmd);
}

// 기기 제어 Turn off
bool MatterController::turnOff(uint64_t nodeId,uint16_t endpointId){
    std::cout << "[MatterController] 🔌 기기 끄기 요청 (NodeID: " << nodeId << ")" << std::endl;

    std::string cmd = mChipToolPath + " onoff off " + std::to_string(nodeId) + " " + std::to_string(endpointId);
    return executeCommand(cmd);
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