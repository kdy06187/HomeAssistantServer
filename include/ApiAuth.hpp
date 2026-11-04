#pragma once
#include <optional>
#include <string>

namespace ApiAuth {
    // 토큰 파일을 읽어 검증한다. 파일이 없거나 안전하지 않으면 nullopt를 반환한다.
    std::optional<std::string> loadToken(const std::string& tokenFilePath);

    // Authorization 헤더 값이 "Bearer <expectedToken>" 형식과 일치하는지 검사한다.
    bool isAuthorized(const std::string& authorizationHeader, const std::string& expectedToken);
}
