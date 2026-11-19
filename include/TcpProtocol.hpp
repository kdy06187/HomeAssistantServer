#pragma once
#include <optional>
#include <string>
#include "../src/json.hpp"

// DIY 기기와 주고받는 TCP 메시지(줄바꿈으로 끝나는 JSON 한 줄).
// 기기 펌웨어가 이 문자열을 그대로 해석하므로 키·값·순서를 바꾸면 안 된다.
namespace TcpProtocol {

// "ON"/"OFF" 같은 단순 명령은 릴레이 제어 요청으로 감싸고, '{'로 시작하면 JSON 그대로 다시 직렬화한다.
// JSON이 잘못됐으면 false
inline bool buildCommandRequest(const std::string& command, nlohmann::json& outRequest) {
    if (!command.empty() && command.front() == '{') {
        try {
            outRequest = nlohmann::json::parse(command);
        } catch (const nlohmann::json::parse_error&) {
            return false;
        }
        return true;
    }
    outRequest = nlohmann::json::object();
    outRequest["action"] = "CONTROL";
    outRequest["target"] = "RELAY";
    outRequest["value"] = command;
    return true;
}

inline nlohmann::json buildGetStateRequest() {
    nlohmann::json request;
    request["action"] = "GET_STATE";
    return request;
}

inline std::string toLine(const nlohmann::json& request) { return request.dump() + "\n"; }

inline const std::string& resetLine() {
    static const std::string line = "{\"action\":\"RESET\"}\n";
    return line;
}

// ---- 응답 해석(예외를 던지지 않는다. 필드 형이 다르면 실패로 본다) ----

// 제어 응답: "status"가 문자열 "success"면 true. 아니면 false이고 outErrorMessage에 사유
inline bool isCommandSuccess(const nlohmann::json& response, std::string& outErrorMessage) {
    if (!response.is_object()) {
        outErrorMessage = "응답 없음";
        return false;
    }
    const auto status = response.find("status");
    if (status != response.end() && status->is_string() && status->get<std::string>() == "success") return true;
    const auto message = response.find("message");
    outErrorMessage = (message != response.end() && message->is_string()) ? message->get<std::string>() : "알 수 없는 에러";
    return false;
}

// 상태 응답: "state"가 문자열이면 그 값, 없거나 문자열이 아니면 nullopt(응답 오류)
inline std::optional<std::string> parseStateResponse(const nlohmann::json& response) {
    if (!response.is_object()) return std::nullopt;
    const auto state = response.find("state");
    if (state == response.end() || !state->is_string()) return std::nullopt;
    return state->get<std::string>();
}

} // namespace TcpProtocol
