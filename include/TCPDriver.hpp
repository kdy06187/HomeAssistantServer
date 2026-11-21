#pragma once
#include "ProtocolDriver.hpp"
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include "IMessageSender.hpp"
#include "TcpServer.hpp"

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
    // IMessageSender 구현
    Status sendTo(const std::string& deviceId, const std::string& jsonLine) override;
    void broadcast(const std::string& jsonLine) override;
    // TCP 서버 시작 및 종료 메서드
    bool startServer(int port, const std::string& bindAddress = "0.0.0.0");  // 포트를 열지 못하면 false
    void stopServer();
private:
    // 소켓 수락·연결 맵·요청/응답은 TcpServer가 맡는다(BLE 없이 테스트하려고 분리)
    TcpServer server_;
};