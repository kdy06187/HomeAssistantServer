#pragma once
#include "ProtocolDriver.hpp"
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include "IMessageSender.hpp"
#include "TcpMessageSender.hpp"
#include "TcpServer.hpp"
#include <optional>

class TCPDriver : public ProtocolDriver, public IMessageSender {
public:
    // Wi-Fi 설정을 보낸 뒤 기기가 접속하기를 기다리는 시간(초). 규칙은 "실기 측정 최댓값×3, 20~30초".
    // 실기(2026-11-23) 3회 2·5·2초 → 5×3=15초가 하한 20초보다 작아 20초
    static constexpr int kCommissionConnectWaitSeconds = 20;

    // pcAgentId: "TCP_" + MAC 12자(PC 종료 메시지를 받을 에이전트), 없으면 모든 연결에 보낸다
    explicit TCPDriver(MacResolver& macResolver, std::optional<std::string> pcAgentId = std::nullopt);
    ~TCPDriver() override;
    // TCPDriver의 sendCommand 메서드 구현
    bool sendCommand(const std::string& deviceId, const std::string& command) override;
    Result<CommissionOutcome> commission(const CommissionRequest& request,
                                         const std::unordered_set<std::string>& activeDeviceIds,
                                         const std::atomic<bool>& cancelRequested) override;
    bool unpairDevice(const std::string& deviceId) override;
    std::string readDeviceState(const std::string& deviceId, bool isManualRequest) override;
    // IMessageSender 구현(TcpMessageSender에 위임)
    Status sendTo(const std::string& deviceId, const std::string& jsonLine) override;
    void sendToPcAgent(const std::string& jsonLine) override;
    // TCP 서버 시작 및 종료 메서드
    bool startServer(int port, const std::string& bindAddress = "0.0.0.0");  // 포트를 열지 못하면 false
    // 기기 연결 알림. startServer 전에 한 번만 설정한다
    void setConnectionListener(TcpServer::ConnectionListener listener) { server_.setConnectionListener(std::move(listener)); }
    void stopServer();
private:
    // BLE로 기기를 찾아 Wi-Fi·서버 주소를 쓴다. 실패는 예외(이유가 정해진 실패는 CommissionAbort)
    // cancelRequested가 서면 CommissionCancelled를 던진다(스캔 1초마다·연결 뒤 확인)
    void sendWifiConfigOverBle(const CommissionRequest& request, const std::string& serverIp, int serverPort,
                               const std::atomic<bool>& cancelRequested);
    // 소켓 수락·연결 맵·요청/응답은 TcpServer가 맡는다(BLE 없이 테스트하려고 분리)
    TcpServer server_;
    TcpMessageSender messageSender_{server_};  // server_ 뒤에 선언해야 먼저 만들어진 서버를 가리킨다
};