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
    // 추후 구현
    return true;
}
// 기기 제어 Turn on
bool MatterController::turnOn(uint64_t nodeId,uint16_t endpointId){
    std::cout << "[MatterController] 기기 켜기 : NodeId=" << nodeId << ", EndpointId=" << endpointId << std::endl;
    // 추후 구현
    return true;
}

// 기기 제어 Turn off
bool MatterController::turnOff(uint64_t nodeId,uint16_t endpointId){
    std::cout << "[MatterController] 기기 끄기 : NodeId=" << nodeId << ", EndpointId=" << endpointId << std::endl;
    // 추후 구현
    return true;
}

// 인터페이스 구현
void MatterController::sendCommand(std::string deviceId, std::string command){
    uint64_t nodeId = std::stoull(deviceId);
    if(command == "ON"){
        turnOn(nodeId,1);
    } else if(command == "OFF"){
        turnOff(nodeId,1);
    }
}