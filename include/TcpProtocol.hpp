#pragma once
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

} // namespace TcpProtocol
