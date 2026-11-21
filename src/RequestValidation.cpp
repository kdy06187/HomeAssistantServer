#include "RequestValidation.hpp"
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace {

bool isControlChar(unsigned char c) {
    return c < 0x20 || c == 0x7f;
}

bool hasControlChar(const std::string& text) {
    for (unsigned char c : text) {
        if (isControlChar(c)) return true;
    }
    return false;
}

bool isUtf8Continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

// UTF-8 글자 수(연속 바이트를 세지 않는다)
size_t countUtf8Chars(const std::string& text) {
    size_t count = 0;
    for (unsigned char c : text) {
        if (!isUtf8Continuation(c)) ++count;
    }
    return count;
}

Result<CommissionInput> invalidCommission(const std::string& reason) {
    return Result<CommissionInput>::failure(ErrorCode::InvalidRequest, reason);
}

Result<HistoryQuery> invalidHistory(const std::string& reason) {
    return Result<HistoryQuery>::failure(ErrorCode::InvalidRequest, reason);
}

bool isAllDigits(const std::string& text) {
    if (text.empty()) return false;
    for (unsigned char c : text) {
        if (!std::isdigit(c)) return false;
    }
    return true;
}

// 수동 설정 코드(숫자 11 또는 21자리) 또는 QR 내용("MT:" + 0-9 A-Z . -)
bool isValidMatterPayload(const std::string& payload) {
    if (isAllDigits(payload)) return payload.size() == 11 || payload.size() == 21;
    if (payload.rfind("MT:", 0) != 0 || payload.size() == 3) return false;
    for (size_t i = 3; i < payload.size(); ++i) {
        const char c = payload[i];
        const bool allowed = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

bool isValidTcpPayload(const std::string& payload) {
    return payload.empty() || RequestValidation::isValidIpv4Address(payload);
}

// 문자열 필드를 읽는다. 없으면 absent, 문자열이 아니면 wrongType.
enum class FieldState { ok, absent, wrongType };

FieldState readString(const nlohmann::json& body, const char* key, std::string& out) {
    const auto it = body.find(key);
    if (it == body.end()) return FieldState::absent;
    if (!it->is_string()) return FieldState::wrongType;
    out = it->get<std::string>();
    return FieldState::ok;
}

std::optional<ProtocolType> parseProtocolType(const std::string& text) {
    if (text == "MATTER") return ProtocolType::MATTER;
    if (text == "TCP_DIY") return ProtocolType::TCP_DIY;
    return std::nullopt;
}

// driverTypes 원소는 1~3만 허용한다(0은 보내지 않는 값)
bool parseDriverTypes(const nlohmann::json& value, std::vector<DriverType>& out) {
    if (!value.is_array()) return false;
    for (const auto& item : value) {
        if (!item.is_number_integer()) return false;
        const auto number = item.get<int64_t>();
        if (number < 1 || number > 3) return false;
        out.push_back(static_cast<DriverType>(number));
    }
    return true;
}

bool isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (month == 2 && isLeapYear(year)) ? 29 : kDays[month - 1];
}

// 1970-01-01부터의 일 수(Howard Hinnant의 days_from_civil)
int64_t dayNumber(const CivilDate& date) {
    const int64_t y = date.month <= 2 ? date.year - 1 : date.year;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yearOfEra = y - era * 400;
    const int64_t monthShifted = date.month > 2 ? date.month - 3 : date.month + 9;
    const int64_t dayOfYear = (153 * monthShifted + 2) / 5 + date.day - 1;
    const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

// 정확히 YYYY-MM-DD이고 달력에 있는 날짜만 허용한다
std::optional<CivilDate> parseDate(const std::string& text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return std::nullopt;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return std::nullopt;
    }
    CivilDate date;
    date.year = std::stoi(text.substr(0, 4));
    date.month = std::stoi(text.substr(5, 2));
    date.day = std::stoi(text.substr(8, 2));
    if (date.year < 1 || date.month < 1 || date.month > 12) return std::nullopt;
    if (date.day < 1 || date.day > daysInMonth(date.year, date.month)) return std::nullopt;
    return date;
}

std::optional<HistoryPeriod> parsePeriod(const std::string& text) {
    if (text == "day") return HistoryPeriod::Day;
    if (text == "week") return HistoryPeriod::Week;
    if (text == "month") return HistoryPeriod::Month;
    return std::nullopt;
}

// 부호 없는 10진수만. 64비트 범위를 넘으면 실패
std::optional<int64_t> parseLimitNumber(const std::string& text) {
    if (!isAllDigits(text)) return std::nullopt;
    errno = 0;
    const long long value = std::strtoll(text.c_str(), nullptr, 10);
    if (errno == ERANGE) return std::nullopt;
    return static_cast<int64_t>(value);
}

}  // namespace

bool RequestValidation::isValidDeviceId(const std::string& deviceId) {
    if (deviceId.empty() || deviceId.size() > 64) return false;
    for (unsigned char c : deviceId) {
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

bool RequestValidation::isValidIpv4Address(const std::string& text) {
    in_addr address{};
    return inet_pton(AF_INET, text.c_str(), &address) == 1;
}

std::string RequestValidation::sanitizeForLog(const std::string& text, size_t maxLength) {
    size_t length = text.size() > maxLength ? maxLength : text.size();
    // 글자 중간에서 잘리면 그 글자를 통째로 버린다
    while (length > 0 && length < text.size() && isUtf8Continuation(static_cast<unsigned char>(text[length]))) {
        --length;
    }
    std::string result = text.substr(0, length);
    for (char& c : result) {
        if (isControlChar(static_cast<unsigned char>(c))) c = '?';
    }
    return result;
}

Result<CommissionInput> RequestValidation::parseCommission(const nlohmann::json& body) {
    if (!body.is_object()) return invalidCommission("본문이 객체가 아님");

    std::string protocolText;
    if (readString(body, "protocolType", protocolText) != FieldState::ok) return invalidCommission("protocolType");
    const auto protocolType = parseProtocolType(protocolText);
    if (!protocolType) return invalidCommission("protocolType 값");

    CommissionInput input;
    input.protocolType = *protocolType;

    if (readString(body, "deviceName", input.deviceName) != FieldState::ok) return invalidCommission("deviceName");
    const size_t nameChars = countUtf8Chars(input.deviceName);
    if (nameChars < 1 || nameChars > kMaxDeviceNameChars) return invalidCommission("deviceName 길이");

    if (readString(body, "payload", input.payload) != FieldState::ok) return invalidCommission("payload");
    const bool payloadOk = *protocolType == ProtocolType::MATTER ? isValidMatterPayload(input.payload)
                                                                 : isValidTcpPayload(input.payload);
    if (!payloadOk) return invalidCommission("payload 형식");

    // 선택 필드: 빠지면 빈 문자열, 있는데 문자열이 아니면 오류
    if (readString(body, "ssid", input.ssid) == FieldState::wrongType) return invalidCommission("ssid");
    if (readString(body, "password", input.password) == FieldState::wrongType) return invalidCommission("password");
    if (input.ssid.size() > kMaxSsidBytes) return invalidCommission("ssid 길이");
    if (input.password.size() > kMaxPasswordBytes) return invalidCommission("password 길이");

    // payload는 위 형식 검사에서 이미 제어문자가 걸러진다
    if (hasControlChar(input.deviceName) || hasControlChar(input.ssid) || hasControlChar(input.password)) {
        return invalidCommission("제어문자");
    }

    const auto typesIt = body.find("driverTypes");
    if (typesIt != body.end() && !parseDriverTypes(*typesIt, input.driverTypes)) {
        return invalidCommission("driverTypes");
    }
    return Result<CommissionInput>::success(std::move(input));
}

Result<HistoryQuery> RequestValidation::parseHistoryQuery(const httplib::Params& params) {
    HistoryQuery query;
    query.limit = kDefaultHistoryLimit;

    const auto find = [&params](const char* key) -> const std::string* {
        const auto it = params.find(key);
        return it == params.end() ? nullptr : &it->second;
    };

    if (const std::string* text = find("from")) {
        query.from = parseDate(*text);
        if (!query.from) return invalidHistory("from 형식");
    }
    if (const std::string* text = find("to")) {
        query.to = parseDate(*text);
        if (!query.to) return invalidHistory("to 형식");
    }
    if (const std::string* text = find("period")) {
        query.period = parsePeriod(*text);
        if (!query.period) return invalidHistory("period 값");
    }
    if (const std::string* text = find("limit")) {
        const auto limit = parseLimitNumber(*text);
        if (!limit || *limit < 1) return invalidHistory("limit 값");
        query.limit = *limit > kMaxHistoryLimit ? kMaxHistoryLimit : *limit;
    }

    if (query.period && (!query.from || !query.to)) return invalidHistory("period에는 from·to 필요");
    if (query.from && query.to) {
        const int64_t days = dayNumber(*query.to) - dayNumber(*query.from);
        if (days < 0) return invalidHistory("from이 to보다 늦음");
        if (days + 1 > kMaxHistoryRangeDays) return invalidHistory("범위 366일 초과");
    }
    return Result<HistoryQuery>::success(std::move(query));
}
