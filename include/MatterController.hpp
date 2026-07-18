#pragma once
#include "ProtocolDriver.hpp"
#include <string>
#include <cstdint>
#include <thread>

// Matter SDK의 핵심 헤더
namespace chip {
    namespace Controller {
        class DeviceCommissioner; // Matter SDK의 커미셔너 전방 선언
    }
}

class MatterController : public ProtocolDriver {
public:
    MatterController();
    ~MatterController();

    // =========================================================
    // 1. 생명주기 (Lifecycle) 관리
    // =========================================================
    /**
     * @brief Matter 스택 및 이벤트 루프를 초기화합니다.
     * @return 성공 여부 (true/false)
     */
    bool Initialize();

    /**
     * @brief Matter 스택을 안전하게 종료하고 메모리를 해제합니다.
     */
    void shutdown();

    // =========================================================
    // 2. 커미셔닝 (기기 등록 - Commissioner 역할)
    // =========================================================
    /**
     * @brief 새로운 Matter 기기를 서버의 네트워크에 등록합니다.
     * @param nodeId 서버가 기기에 부여할 고유 ID (예: 1, 2, 3...)
     * @param setupPinCode 기기의 11자리 핀 코드 (예: 34460414140)
     * @param discriminator 기기의 4자리 식별자
     * @return 커미셔닝 성공 여부
     */
    bool commissionDevice(uint64_t nodeId, uint32_t setupPinCode, uint16_t discriminator);

    // =========================================================
    // 3. 기기 제어 (제어 - Controller 역할)
    // =========================================================
    bool turnOn(uint64_t nodeId,uint16_t endpointId);
    bool turnOff(uint64_t nodeId,uint16_t endpointId);

    // 명령어 전송 인터페이스 구현
    void sendCommand(std::string deviceId, std::string command) override;

private:
    bool mIsInitialized;

    void RunEventLoop(); // Matter 이벤트 루프를 실행하는 내부 메서드

    std::thread mEventLoopThread; // 이벤트 루프를 별도의 스레드에서 실행

    // Matter SDK에서 실제로 기기 등록과 통신을 담당할 핵심 객체 포인터
    chip::Controller::DeviceCommissioner* mCommissioner;

    // 내부적으로 이벤트를 처리하거나 콜백을 받을 프라이빗 메서드들
    // void onDeviceConnected(...);
};