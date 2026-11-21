#pragma once
#include <optional>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace ApiAuth {
    // 토큰 파일을 읽어 검증한다. 파일이 없거나 안전하지 않으면 nullopt를 반환한다.
    // 파일 소유자가 expectedOwner(기본: 실행 계정)가 아니면 경고만 남기고 토큰은 돌려준다.
    std::optional<std::string> loadToken(const std::string& tokenFilePath, uid_t expectedOwner = geteuid());

    // 기대 토큰이 빈 문자열이면 어떤 헤더도 거부한다(설정 누락으로 인증이 열리는 것을 막는다).
    // Authorization 헤더 값이 "Bearer <expectedToken>" 형식과 일치하는지 검사한다.
    bool isAuthorized(const std::string& authorizationHeader, const std::string& expectedToken);
}
