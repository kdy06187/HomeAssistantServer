#include "ErrorMapping.hpp"
#include "json.hpp"

HttpErrorInfo ErrorMapping::describe(ErrorCode code) {
    switch (code) {
        case ErrorCode::InvalidRequest:
            return {400, "INVALID_REQUEST", "요청 형식이 올바르지 않습니다."};
        case ErrorCode::UnsupportedOperation:
            return {400, "UNSUPPORTED_OPERATION", "이 기기에서 지원하지 않는 요청입니다."};
        case ErrorCode::Unauthorized:
            return {401, "UNAUTHORIZED", "인증에 실패했습니다."};
        case ErrorCode::NotFound:
            return {404, "NOT_FOUND", "없는 경로입니다."};
        case ErrorCode::DeviceNotFound:
            return {404, "DEVICE_NOT_FOUND", "등록되지 않은 기기입니다."};
        case ErrorCode::JobNotFound:
            return {404, "JOB_NOT_FOUND", "없는 등록 작업입니다."};
        case ErrorCode::CommandBlocked:
            return {409, "COMMAND_BLOCKED", "기기 드라이버가 명령을 막았습니다."};
        case ErrorCode::CommissionBusy:
            return {409, "COMMISSION_BUSY", "다른 기기 등록이 진행 중입니다."};
        case ErrorCode::DeviceUnreachable:
            return {502, "DEVICE_UNREACHABLE", "기기가 응답하지 않습니다."};
        case ErrorCode::CommissionFailed:
            return {200, "COMMISSION_FAILED", "기기 등록에 실패했습니다."};
        case ErrorCode::CommissionTimeout:
            return {200, "COMMISSION_TIMEOUT", "기기 등록 시간이 초과되었습니다."};
        case ErrorCode::Cancelled:
            return {200, "CANCELLED", "서버 종료로 취소되었습니다."};
        case ErrorCode::AlreadyRegistered:
            return {200, "ALREADY_REGISTERED", "이미 등록된 기기입니다."};
        case ErrorCode::None:
        case ErrorCode::Internal:
            break;
    }
    return {500, "INTERNAL_ERROR", "서버 내부 오류입니다."};
}

nlohmann::json ErrorMapping::buildErrorJson(ErrorCode code) {
    const HttpErrorInfo info = describe(code);
    nlohmann::json body;
    body["error"]["code"] = info.code;
    body["error"]["message"] = info.message;
    return body;
}

std::string ErrorMapping::buildErrorBody(ErrorCode code) {
    return buildErrorJson(code).dump();
}
