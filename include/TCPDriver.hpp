#pragma once
#include "ProtocolDriver.hpp"
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>

class TCPDriver : public ProtocolDriver {
public:
    TCPDriver(DeviceManager& deviceManager);
    ~TCPDriver() override;
    // TCPDriver의 sendCommand 메서드 구현
    bool sendCommand(std::string deviceId, std::string command) override;
    bool commissionDevice(std::string name, std::string payload) override;
    bool unpairDevice(std::string deviceId) override;
    std::string readDeviceState(std::string deviceId) override;
    // TCP 서버 시작 및 종료 메서드
    void startServer(int port);
    void stopServer();
private:
    // TCP 서버의 클라이언트 연결을 수락하는 루프 메서드
    void accecptLoop(int port);

    std::thread server_thread_; // TCP 서버를 실행하는 스레드
    std::atomic<bool> running_; // 서버 구동 상태 플래그
    //연결된 기기들의 소켓 파이프번호를 기억하는 맵
    std::unordered_map<std::string,int> client_sockets_; 
    // 소켓 맵 보호
    std::mutex sockets_mutex_;
};