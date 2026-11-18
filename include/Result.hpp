#pragma once
#include <optional>
#include <string>
#include <utility>

enum class ErrorCode {
    None, InvalidRequest, UnsupportedOperation, Unauthorized, NotFound,
    DeviceNotFound, JobNotFound, CommandBlocked, CommissionBusy, DeviceUnreachable,
    CommissionFailed, CommissionTimeout, Cancelled, AlreadyRegistered, Internal
};

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message; // 로그 전용. 외부 응답에 싣지 않는다.
};

struct Status {
    Error error;

    bool ok() const { return error.code == ErrorCode::None; }

    static Status success() { return Status{}; }
    static Status failure(ErrorCode code, std::string message) {
        return Status{Error{code, std::move(message)}};
    }
};

template <typename T>
struct Result {
    std::optional<T> value;
    Error error;

    bool ok() const { return value.has_value(); }

    static Result success(T newValue) {
        Result result;
        result.value = std::move(newValue);
        return result;
    }
    static Result failure(ErrorCode code, std::string message) {
        Result result;
        result.error = Error{code, std::move(message)};
        return result;
    }
};
