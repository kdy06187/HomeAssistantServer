#pragma once
#include <optional>
#include <string>
#include "IMessageSender.hpp"
#include "Result.hpp"

class TcpServer;

// DIY 기기 연결(TcpServer)로 메시지를 보낸다. PC 종료 메시지를 누구에게 보낼지 여기서 정한다:
// PC 에이전트 ID가 설정됐으면 그 연결에만, 설정이 없으면 v1.0처럼 모든 연결에.
// TCPDriver(SimpleBLE 의존)와 떼어 놓아 실제 TcpServer로 자동 시험한다.
class TcpMessageSender : public IMessageSender {
public:
    explicit TcpMessageSender(TcpServer& server) : server_(server) {}

    // 성공 응답("status":"success")이면 성공, 연결 없음·무응답·오류 응답이면 DeviceUnreachable
    Status sendTo(const std::string& deviceId, const std::string& jsonLine) override;
    // 결과는 로그로만 남긴다. 에이전트에게 못 보내도 호출자(PCDriver)는 정해진 지연 뒤 전원을 끈다
    void sendToPcAgent(const std::string& jsonLine) override;

private:
    TcpServer& server_;
};

namespace TcpAgentConfig {
// HAC_PC_AGENT_MAC 값 해석. 없거나 빈 값 → 성공(값 없음, 모든 연결에 보냄),
// MAC 형식 → 성공("TCP_" + 12자), 그 밖 → 실패(서버를 시작하지 않는다)
Result<std::optional<std::string>> parsePcAgentId(const char* environmentValue);
}  // namespace TcpAgentConfig
