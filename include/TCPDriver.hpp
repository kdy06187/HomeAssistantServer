#pragma once
#include "ProtocolDriver.hpp"
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include "IMessageSender.hpp"
#include <atomic>
#include "../src/json.hpp"

class TCPDriver : public ProtocolDriver, public IMessageSender {
public:
    TCPDriver();
    ~TCPDriver() override;
    // TCPDriver의 sendCommand 메서드 구현
    bool sendCommand(const std::string& deviceId, const std::string& command) override;
    Result<CommissionOutcome> commission(const CommissionRequest& request,
                                         const std::unordered_set<std::string>& activeDeviceIds) override;
    bool unpairDevice(const std::string& deviceId) override;
    std::string readDeviceState(const std::string& deviceId, bool isManualRequest) override;
    //IMessageSender의 SendCommandTo 메서드 구현
    void SendCommandTo(const std::string& target_id, const std::string& command) override;
    // TCP 서버 시작 및 종료 메서드
    void startServer(int port);
    void stopServer();
private:
    // TCP 서버의 클라이언트 연결을 수락하는 루프 메서드
    void accecptLoop(int port);
    nlohmann::json sendAndReceive(const std::string& devcieId, const nlohmann::json& requestJson);
    std::thread server_thread_; // TCP 서버를 실행하는 스레드
    std::atomic<bool> running_; // 서버 구동 상태 플래그
    //연결된 기기들의 소켓 파이프번호를 기억하는 맵
    std::unordered_map<std::string,int> client_sockets_; 
    // 소켓 맵 보호
    std::mutex sockets_mutex_;
};