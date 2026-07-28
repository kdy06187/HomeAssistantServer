#include "MatterController.hpp"

#include <iostream>



using namespace chip;
using namespace chip::Controller;
using namespace chip::app::Clusters;

MatterController::MatterController()
    : mIsInitialized(false),
    mCommissioner(nullptr),
    mOnDeviceConnectedCallback(OnDeviceConnected, this),
    mOnDeviceConnectionFailureCallback(OnDeviceConnectionFailure, this){
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

    mCommissioner = new chip::Controller::DeviceCommissioner;
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
    (void)chip::DeviceLayer::PlatformMgr().StopEventLoopTask();
    if(mEventLoopThread.joinable()){
        mEventLoopThread.join();
    }
    chip::DeviceLayer::PlatformMgr().Shutdown();
    chip::Platform::MemoryShutdown();

    mIsInitialized = false;
    std::cout << "[MatterController] 스레드 종료 성공 " << std::endl;
}

//기기 등록
bool MatterController::commissionDevice(uint64_t nodeId, const std::string& manualPincode){
    std::cout << "[MatterController] 기기 등록 시도: NodeId=" << nodeId << ", PinCode=" << manualPincode << std::endl;
    if (!mIsInitialized) {
        std::cerr << "[MatterController] ❌ 에러: Matter 스택이 초기화되지 않았습니다!" << std::endl;
        return false;
    }
    std::string chipToolPath = "/home/kdy/home-assistant-core/third_party/connectedhomeip/out/host/chip-tool";
    
    std::string cmd = chipToolPath + " pairing code " + std::to_string(nodeId) + " " + manualPincode;
    std::cout << " > Executing: " << cmd << std::endl;
    int result = std::system(cmd.c_str());
    if (result == 0) {
        std::cout << "✅ [MatterController] 보안 패브릭 등록 및 페어링 성공!" << std::endl;
        return true;
    } else {
        std::cerr << "⚠️ [MatterController] 페어링 명령 실패 (이미 등록되었거나 기기가 오프라인일 수 있음)" << std::endl;
        return false;
    }

    // CHIP_ERROR err = mCommissioner->PairDevice(nodeId, manualPincode.c_str());
    // if (err == CHIP_NO_ERROR) {
    //     std::cout << "[MatterController] ✅ 기기 커미셔닝(페어링) 요청 성공! " << std::endl;
    //     std::cout << " -> 비동기 등록 절차(PASE 보안 세션)가 백그라운드에서 진행됩니다. (Target NodeId: " << nodeId << ")" << std::endl;
    //     return true;
    // } else {
    //     std::cerr << "[MatterController] ❌ 기기 커미셔닝 요청 실패. Error: " << err.Format() << std::endl;
    //     return false;
    // }

    // return true;
}
// 기기 제어 Turn on
bool MatterController::turnOn(uint64_t nodeId,uint16_t endpointId){
    std::cout << "[MatterController] 💡 기기 켜기 요청 전송 중..." << std::endl;
    std::cout << " -> Target NodeId: " << nodeId << ", EndpointId: " << endpointId << std::endl;

    if (!mIsInitialized || !mCommissioner) {
        std::cerr << "[MatterController] ❌ 에러: Matter 스택이 초기화되지 않았습니다!" << std::endl;
        return false;
    }
    // 콜백에서 사용할 수 있도록 현재 명령 상태 저장
    mCurrentEndpointId = endpointId;
    mIsTurnOnCommand = true;

    CHIP_ERROR err = mCommissioner->GetConnectedDevice(nodeId, &mOnDeviceConnectedCallback, &mOnDeviceConnectionFailureCallback);
    if(err != CHIP_NO_ERROR){
        std::cerr <<"[MatterController] ❌ 기기 켜기 명령 전송 실패. Error Code: " << err.Format() << std::endl;
        return false;
    }

    return true;
}

// 기기 제어 Turn off
bool MatterController::turnOff(uint64_t nodeId,uint16_t endpointId){
    std::cout << "[MatterController] 🔌 기기(" << nodeId << ") 세션 연결 요청 중..." << std::endl;

    if (!mIsInitialized || !mCommissioner) {
        std::cerr << "[MatterController] ❌ 에러: Matter 스택이 초기화되지 않았습니다!" << std::endl;
        return false;
    }

    mCurrentEndpointId = endpointId;
    mIsTurnOnCommand = false; // Off 명령임을 명시
    CHIP_ERROR err = mCommissioner->GetConnectedDevice(nodeId, &mOnDeviceConnectedCallback, &mOnDeviceConnectionFailureCallback);
    return err == CHIP_NO_ERROR;
}

void MatterController::OnDeviceConnected(void* context, chip::Messaging::ExchangeManager& exchangeMgr, const chip::SessionHandle& sessionHandle){
    MatterController* self = static_cast<MatterController*>(context);
    std::cout << "[MatterController] 암호화 세션 획득. 실제 제어 명령 발송..." << std::endl;

    // SDK에서 명령 성공/실패 시 불려질 인라인 람다(Lambda) 함수
    auto onSuccess = [](const chip::app::ConcreteCommandPath& path, const chip::app::StatusIB& status, const auto& dataResponse) {
        std::cout << "[MatterController] 기기 제어 완료 (ACK 수신 성공!)" << std::endl;
    };

    auto onFailure = [](CHIP_ERROR error) {
        std::cerr << "[MatterController] 명령 실행 실패 (Timeout 등): " << error.Format() << std::endl;
    };

    // 저장해둔 플래그에 따라 On 또는 Off 클러스터 명령 생성 및 발송
    if (self->mIsTurnOnCommand) {
        OnOff::Commands::On::Type onCommand;
        (void)chip::Controller::InvokeCommandRequest(&exchangeMgr, sessionHandle, self->mCurrentEndpointId, onCommand, onSuccess, onFailure);
    } else {
        OnOff::Commands::Off::Type offCommand;
        (void)chip::Controller::InvokeCommandRequest(&exchangeMgr, sessionHandle, self->mCurrentEndpointId, offCommand, onSuccess, onFailure);
    }
}
void MatterController::OnDeviceConnectionFailure(void* context, const chip::ScopedNodeId& peerId, CHIP_ERROR error) {
    std::cerr << "[MatterController] 기기와 통신 불가 (오프라인 상태일 수 있음). Error: " << error.Format() << std::endl;
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