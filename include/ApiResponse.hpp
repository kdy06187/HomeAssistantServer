#pragma once
#include <utility>
#include "ErrorMapping.hpp"
#include "../src/json.hpp"

// 라우트 핸들러가 돌려주는 값. 직렬화·Content-Type·예외 처리는 HTTPServer의 addRoute 래퍼가 맡는다.
struct ApiResponse {
    int httpStatus = 200;
    nlohmann::json body;

    static ApiResponse ok(nlohmann::json body, int httpStatus = 200) {
        return ApiResponse{httpStatus, std::move(body)};
    }
    // 상태 코드와 {"error":{"code","message"}} 본문은 ErrorMapping이 정한다
    static ApiResponse error(ErrorCode code) {
        const HttpErrorInfo info = ErrorMapping::describe(code);
        return ApiResponse{info.httpStatus, ErrorMapping::buildErrorJson(info)};
    }
};
