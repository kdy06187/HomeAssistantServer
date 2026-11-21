#pragma once
#include <string>
#include "Result.hpp"

// ErrorCode를 HTTP 응답(상태·코드 문자열·고정 문구)으로 바꾼다. 상세 메시지는 응답에 싣지 않는다.
struct HttpErrorInfo {
    int httpStatus;
    std::string code;
    std::string message;
};

namespace ErrorMapping {
    // 등록 작업(E8) 본문 안에서만 쓰는 코드는 HTTP 상태를 200으로 둔다.
    // None은 오류가 아니므로 호출하면 안 되지만, 안전하게 500으로 처리한다.
    HttpErrorInfo describe(ErrorCode code);

    // {"error":{"code":...,"message":...}} 본문
    std::string buildErrorBody(ErrorCode code);
}
