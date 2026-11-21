#include <gtest/gtest.h>
#include "ErrorMapping.hpp"
#include "json.hpp"

namespace {

struct ExpectedRow {
    ErrorCode code;
    int httpStatus;
    const char* codeText;
    const char* message;
};

const ExpectedRow kRows[] = {
    {ErrorCode::InvalidRequest, 400, "INVALID_REQUEST", "요청 형식이 올바르지 않습니다."},
    {ErrorCode::UnsupportedOperation, 400, "UNSUPPORTED_OPERATION", "이 기기에서 지원하지 않는 요청입니다."},
    {ErrorCode::Unauthorized, 401, "UNAUTHORIZED", "인증에 실패했습니다."},
    {ErrorCode::NotFound, 404, "NOT_FOUND", "없는 경로입니다."},
    {ErrorCode::DeviceNotFound, 404, "DEVICE_NOT_FOUND", "등록되지 않은 기기입니다."},
    {ErrorCode::JobNotFound, 404, "JOB_NOT_FOUND", "없는 등록 작업입니다."},
    {ErrorCode::CommandBlocked, 409, "COMMAND_BLOCKED", "기기 드라이버가 명령을 막았습니다."},
    {ErrorCode::CommissionBusy, 409, "COMMISSION_BUSY", "다른 기기 등록이 진행 중입니다."},
    {ErrorCode::Internal, 500, "INTERNAL_ERROR", "서버 내부 오류입니다."},
    {ErrorCode::DeviceUnreachable, 502, "DEVICE_UNREACHABLE", "기기가 응답하지 않습니다."},
    {ErrorCode::CommissionFailed, 200, "COMMISSION_FAILED", "기기 등록에 실패했습니다."},
    {ErrorCode::CommissionTimeout, 200, "COMMISSION_TIMEOUT", "기기 등록 시간이 초과되었습니다."},
    {ErrorCode::Cancelled, 200, "CANCELLED", "서버 종료로 취소되었습니다."},
    {ErrorCode::AlreadyRegistered, 200, "ALREADY_REGISTERED", "이미 등록된 기기입니다."},
};

TEST(ErrorMappingDescribe, EveryRowMatchesTheDesignTable) {
    for (const auto& row : kRows) {
        const HttpErrorInfo info = ErrorMapping::describe(row.code);
        EXPECT_EQ(info.httpStatus, row.httpStatus) << row.codeText;
        EXPECT_EQ(info.code, row.codeText);
        EXPECT_EQ(info.message, row.message) << row.codeText;
    }
}

TEST(ErrorMappingDescribe, NoneFallsBackToInternalError) {
    const HttpErrorInfo info = ErrorMapping::describe(ErrorCode::None);
    EXPECT_EQ(info.httpStatus, 500);
    EXPECT_EQ(info.code, "INTERNAL_ERROR");
}

TEST(ErrorMappingBody, HasExactlyCodeAndMessageUnderError) {
    const auto body = nlohmann::json::parse(ErrorMapping::buildErrorBody(ErrorCode::DeviceNotFound));
    ASSERT_TRUE(body.is_object());
    EXPECT_EQ(body.size(), 1u);
    ASSERT_TRUE(body["error"].is_object());
    EXPECT_EQ(body["error"].size(), 2u);
    EXPECT_EQ(body["error"]["code"], "DEVICE_NOT_FOUND");
    EXPECT_EQ(body["error"]["message"], "등록되지 않은 기기입니다.");
}

}  // namespace
