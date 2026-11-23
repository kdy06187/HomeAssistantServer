#include "TcpMessageSender.hpp"
#include "MacResolver.hpp"
#include "TcpProtocol.hpp"
#include "TcpServer.hpp"
#include <iostream>

using json = nlohmann::json;

Status TcpMessageSender::sendTo(const std::string& deviceId, const std::string& jsonLine) {
    // '{'로 시작하면 JSON 그대로, "ON"/"OFF" 같은 단순 명령은 릴레이 제어 패킷으로 감싼다(하위 호환)
    json request;
    if (!TcpProtocol::buildCommandRequest(jsonLine, request)) {
        std::cerr << "[TCPDriver] 명령어 JSON 파싱 실패" << std::endl;
        return Status::failure(ErrorCode::InvalidRequest, "명령 JSON 형식 오류");
    }
    const json response = server_.sendAndReceive(deviceId, request);
    if (response.is_null()) {
        std::cerr << "[TCPDriver] 명령 전송 실패 (기기 오프라인): " << deviceId << std::endl;
        return Status::failure(ErrorCode::DeviceUnreachable, "기기 응답 없음");
    }
    // 응답 패킷의 "status"가 "success"인지 확인(필드 형이 달라도 예외 없이 실패)
    std::string errorMessage;
    if (TcpProtocol::isCommandSuccess(response, errorMessage)) {
        std::cout << "[TCPDriver] 명령 전송 성공: " << deviceId << std::endl;
        return Status::success();
    }
    std::cerr << "[TCPDriver] 기기에서 에러 반환: " << errorMessage << std::endl;
    return Status::failure(ErrorCode::DeviceUnreachable, "기기 오류 응답");
}

void TcpMessageSender::sendToPcAgent(const std::string& jsonLine) {
    const auto& agentId = server_.pcAgentId();
    if (!agentId) {
        // 설정이 없으면 v1.0 동작: 연결 목록 사본을 순회해 모두에게 보낸다(시작 때 경고를 남겼다)
        for (const auto& id : server_.connectedIds()) sendTo(id, jsonLine);
        return;
    }
    // 어느 실패 경로든 끝에 차단 예고를 붙인다(전원 차단 자체는 결과와 무관하게 45초 뒤 진행)
    if (!server_.peerIpOf(*agentId)) {
        std::cerr << "[TCPDriver] PC 에이전트(" << *agentId << ") 연결 없음 — 종료 메시지 못 보냄, 45초 뒤 전원 차단" << std::endl;
        return;
    }
    // 에이전트가 방금 끊겼으면(FIN 도착) 서버는 아직 맵에 연결을 들고 있다. 1초 응답 대기 없이 바로 알린다
    if (server_.dropIfPeerGone(*agentId)) {
        std::cerr << "[TCPDriver] PC 에이전트(" << *agentId << ") 연결 끊김 — 종료 메시지 못 보냄, 45초 뒤 전원 차단" << std::endl;
        return;
    }
    // 정전처럼 끊김 신호 없이 사라졌거나 확인 뒤 끊긴 경우는 아래 1초 응답 대기에서 드러난다
    if (sendTo(*agentId, jsonLine).ok()) {
        std::cout << "[TCPDriver] PC 에이전트(" << *agentId << ")가 종료 메시지를 받음" << std::endl;
    } else {
        std::cerr << "[TCPDriver] PC 에이전트(" << *agentId << ") 종료 메시지 응답 없음 또는 오류 — 45초 뒤 전원 차단" << std::endl;
    }
}

Result<std::optional<std::string>> TcpAgentConfig::parsePcAgentId(const char* environmentValue) {
    using AgentIdResult = Result<std::optional<std::string>>;
    if (environmentValue == nullptr || environmentValue[0] == '\0') return AgentIdResult::success(std::nullopt);
    const auto mac = MacResolverParse::normalizeMac(environmentValue);
    if (!mac) return AgentIdResult::failure(ErrorCode::InvalidRequest, "MAC 주소 형식이 아님");
    // 전부 0·전부 F(브로드캐스트)는 실제 기기 MAC이 아니라 어떤 연결과도 맞지 않는다 → 오타로 보고 시작하지 않는다
    if (*mac == "000000000000" || *mac == "FFFFFFFFFFFF") {
        return AgentIdResult::failure(ErrorCode::InvalidRequest, "기기 MAC이 아님(전부 0 또는 FF)");
    }
    return AgentIdResult::success(makeTcpDeviceId(*mac));
}
