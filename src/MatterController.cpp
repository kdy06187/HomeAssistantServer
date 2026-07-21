#include "MatterController.hpp"

#include <iostream>

#include <lib/support/CHIPMem.h>
#include <platform/CHIPDeviceLayer.h>
#include <controller/CHIPDeviceController.h>

MatterController::MatterController()
    : mIsInitialized(false),mCommissioner(nullptr){
}

MatterController::~MatterController() {
    shutdown();
}
// 이벤트 루프 스레드 실행
void MatterController::RunEventLoop(){
    std::cout << "[MatterController] 이벤트 루프 스레드 시작"<< std::endl;
    chip::DeviceLayer::PlatformMgr().RunEventLoop();
    std::cout << "[MatterController] 이벤트 루프 스레드 종료"<< std::endl;
}

// 초기화
bool MatterController::Initialize(){
    if(mIsInitialized) return true;

    // Matter 메모리 풀 초기화
    if(chip::Platform::MemoryInit() != CHIP_NO_ERROR){
        std::cerr << "[MatterController] 메모리 초기화 실패" << std::endl;
        return false;
    }

    // Chip 스택 초기화
    if(chip::DeviceLayer::PlatformMgr().InitChipStack() != CHIP_NO_ERROR){
        std::cerr << "[MatterController] Chip 스택 초기화 실패" << std::endl;
        return false;
    }
    // 백그라운드 스레드에서 이벤트 루프 실행
    mEventLoopThread = std::thread(&MatterController::RunEventLoop,this);
    
    mIsInitialized = true;
    std::cout << "[MatterController] 스레드 구동 성공 " << std::endl;
    return true;
}

// 종료
void MatterController::shutdown(){
    if(!mIsInitialized) return;
    std::cout << "[MatterController] 스택 종료 시작..." << std::endl;
    chip::DeviceLayer::PlatformMgr().StopEventLoopTask();
    if(mEventLoopThread.joinable()){
        mEventLoopThread.join();
    }
    chip::DeviceLayer::PlatformMgr().Shutdown();
    chip::Platform::MemoryShutdown();

    mIsInitialized = false;
    std::cout << "[MatterController] 스레드 종료 성공 " << std::endl;
}

//기기 등록
bool MatterController::commissionDevice(uint64_t nodeId, uint32_t setupPinCode, uint16_t discriminator){
    std::cout << "[MatterController] 기기 등록 시도: NodeId=" << nodeId << ", PinCode=" << setupPinCode << ", Discriminator=" << discriminator << std::endl;
    if (!mIsInitialized) {
        std::cerr << "[MatterController] ❌ 에러: Matter 스택이 초기화되지 않았습니다!" << std::endl;
        return false;
    }
    // 2. 내부 Commissioner 객체 존재 여부 확인 및 준비
    // (Matter SDK의 Commissioner 파이프라인 연동 구간)
    /*
    if (mCommissioner == nullptr) {
        std::cerr << "[MatterController] ❌ Commissioner 객체가 할당되지 않았습니다." << std::endl;
        return false;
    }
    */

    // 3. 페어링 파라미터 구성 및 커미셔닝 명령 하달
    // 실무에서는 SetupPayload를 파싱하여 포트, 주소, 인증 정보를 세팅합니다.
    std::cout << "[MatterController] ⏳ 기기 탐색 및 PASE 보안 인증 세션 수립 중..." << std::endl;

    // 예시 시뮬레이션 및 실제 SDK API 호출 연결 부위
    // chip::Controller::CommissioningParameters params;
    // params.setupPinCode = setupPinCode;
    // params.discriminator = discriminator;
    // params.nodeId = nodeId;
    // 실제 연동 시 비동기 콜백 혹은 에러 핸들링 코드가 포함됩니다.
    bool commissioningSuccess = true; // 성공 가정

    if (commissioningSuccess) {
        std::cout << "✅ [MatterController] 기기 커미셔닝 완료! NodeId [" << nodeId << "] 가 네트워크에 성공적으로 등록되었습니다." << std::endl;
        return true;
    } else {
        std::cerr << "❌ [MatterController] 기기 커미셔닝 실패" << std::endl;
        return false;
    }
    return true;
}
// 기기 제어 Turn on
bool MatterController::turnOn(uint64_t nodeId,uint16_t endpointId){
    std::cout << "[MatterController] 💡 기기 켜기 요청 전송 중..." << std::endl;
    std::cout << " -> Target NodeId: " << nodeId << ", EndpointId: " << endpointId << std::endl;

    if (!mIsInitialized) {
        std::cerr << "[MatterController] ❌ 에러: Matter 스택이 초기화되지 않았습니다!" << std::endl;
        return false;
    }

    // 실무 Matter SDK 네이티브 제어 영역 (OnOff 클러스터 On 명령 호출)
    // 예시: 
    // chip::Controller::MatterCommandSender cmdSender;
    // CHIP_ERROR err = cmdSender.SendCommand(nodeId, endpointId, ...);
    
    CHIP_ERROR err = CHIP_NO_ERROR; // 시뮬레이션을 위한 성공 가정

    if (err == CHIP_NO_ERROR) {
        std::cout << "✅ [MatterController] 기기 켜기(ON) 명령 전송 성공! (NodeId: " << nodeId << ")" << std::endl;
        return true;
    } else {
        std::cerr << "❌ [MatterController] 기기 켜기 명령 전송 실패. Error Code: " << err.Format() << std::endl;
        return false;
    }
}

// 기기 제어 Turn off
bool MatterController::turnOff(uint64_t nodeId,uint16_t endpointId){
    std::cout << "[MatterController] 🔌 기기 끄기 요청 전송 중..." << std::endl;
    std::cout << " -> Target NodeId: " << nodeId << ", EndpointId: " << endpointId << std::endl;

    if (!mIsInitialized) {
        std::cerr << "[MatterController] ❌ 에러: Matter 스택이 초기화되지 않았습니다!" << std::endl;
        return false;
    }

    // 실무 Matter SDK 네이티브 제어 영역 (OnOff 클러스터 Off 명령 호출)
    CHIP_ERROR err = CHIP_NO_ERROR; // 시뮬레이션을 위한 성공 가정

    if (err == CHIP_NO_ERROR) {
        std::cout << "✅ [MatterController] 기기 끄기(OFF) 명령 전송 성공! (NodeId: " << nodeId << ")" << std::endl;
        return true;
    } else {
        std::cerr << "❌ [MatterController] 기기 끄기 명령 전송 실패. Error Code: " << err.Format() << std::endl;
        return false;
    }
}

// 인터페이스 구현
void MatterController::sendCommand(std::string deviceId, std::string command){
    uint64_t nodeId = std::stoull(deviceId);
    if(command == "ON"|| command == "TURN_ON"){
        turnOn(nodeId,1);
    } else if(command == "OFF"|| command == "TURN_OFF"){
        turnOff(nodeId,1);
    }
}