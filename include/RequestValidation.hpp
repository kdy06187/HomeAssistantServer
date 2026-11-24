#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "Device.hpp"
#include "DriverType.hpp"
#include "Result.hpp"
#include "../src/httplib.h"
#include "../src/json.hpp"

// 외부에서 들어온 값을 검사하는 순수 함수 모음. 예외를 던지지 않고 Result로 돌려준다.

struct CommissionInput {
    ProtocolType protocolType = ProtocolType::UNKNOWN;
    std::string deviceName;
    std::string payload;
    std::string ssid;
    std::string password;
    std::vector<DriverType> driverTypes;
};

struct CivilDate {
    int year = 0;
    int month = 0;
    int day = 0;
};

enum class HistoryPeriod { Day, Week, Month };

struct HistoryQuery {
    std::optional<CivilDate> from;
    std::optional<CivilDate> to;
    std::optional<HistoryPeriod> period;
    int64_t limit = 50;
};

namespace RequestValidation {
    constexpr size_t kMaxDeviceNameChars = 64;
    constexpr size_t kMaxSsidBytes = 32;
    constexpr size_t kMaxPasswordBytes = 63;
    constexpr int64_t kDefaultHistoryLimit = 50;
    constexpr int64_t kMaxHistoryLimit = 1000;
    constexpr int kMaxHistoryRangeDays = 366;
    constexpr int kMinHistoryYear = 2000;
    constexpr int kMaxHistoryYear = 2100;

    // [A-Za-z0-9_-]{1,64}
    bool isValidDeviceId(const std::string& deviceId);

    // 점으로 구분한 IPv4 문자열(예 192.168.0.5)인지
    bool isValidIpv4Address(const std::string& text);

    // 사설(10/8·172.16/12·192.168/16)·링크 로컬(169.254/16) IPv4인지. 루프백·공인·0.0.0.0 등은 false
    bool isLanIpv4Address(const std::string& text);

    // 등록 작업 ID 경로 형식 [a-z0-9-]{1,32}
    bool isValidJobId(const std::string& jobId);

    // HAC_HTTP_BIND·HAC_TCP_BIND 같은 바인딩 주소 환경 변수 값을 해석한다.
    // 없거나 빈 문자열이면 기본 0.0.0.0, IPv4 형식이 아니면 InvalidRequest
    Result<std::string> parseBindAddress(const char* environmentValue);

    // 제어문자를 ?로 바꾸고 maxLength 바이트로 자른다(UTF-8 글자 중간은 자르지 않음)
    std::string sanitizeForLog(const std::string& text, size_t maxLength = 200);

    Result<CommissionInput> parseCommission(const nlohmann::json& body);
    Result<HistoryQuery> parseHistoryQuery(const httplib::Params& params);
}
