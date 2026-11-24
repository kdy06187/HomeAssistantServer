#include <gtest/gtest.h>
#include "RequestValidation.hpp"

using nlohmann::json;
using namespace RequestValidation;

namespace {

// ---- deviceId ----
TEST(DeviceIdValidation, AcceptsAllowedCharactersUpTo64) {
    EXPECT_TRUE(isValidDeviceId("TCP_A1B2C3D4E5F6"));
    EXPECT_TRUE(isValidDeviceId("SN-12345"));
    EXPECT_TRUE(isValidDeviceId(std::string(64, 'a')));
}

TEST(DeviceIdValidation, RejectsEmptyTooLongAndOtherCharacters) {
    EXPECT_FALSE(isValidDeviceId(""));
    EXPECT_FALSE(isValidDeviceId(std::string(65, 'a')));
    EXPECT_FALSE(isValidDeviceId("a b"));
    EXPECT_FALSE(isValidDeviceId("a/b"));
    EXPECT_FALSE(isValidDeviceId("a.b"));
    EXPECT_FALSE(isValidDeviceId("기기"));
    EXPECT_FALSE(isValidDeviceId(std::string("a\nb")));
}

// ---- sanitizeForLog ----
TEST(SanitizeForLog, ReplacesControlCharactersWithQuestionMark) {
    EXPECT_EQ(sanitizeForLog(std::string("a\r\nb\tc\x01" "d\x7f" "e", 10)), "a??b?c?d?e");
    EXPECT_EQ(sanitizeForLog(std::string("x\0y", 3)), "x?y");
}

TEST(SanitizeForLog, CutsAt200BytesByDefault) {
    EXPECT_EQ(sanitizeForLog(std::string(500, 'a')).size(), 200u);
    EXPECT_EQ(sanitizeForLog(std::string(200, 'a')).size(), 200u);
}

TEST(SanitizeForLog, DoesNotSplitUtf8Character) {
    // '가'는 3바이트. 4바이트로 자르면 두 번째 글자 중간이라 한 글자만 남는다
    EXPECT_EQ(sanitizeForLog("가나다", 4), "가");
}

TEST(SanitizeForLog, KeepsPlainTextAndKorean) {
    EXPECT_EQ(sanitizeForLog("GET /api/기기"), "GET /api/기기");
}

// ---- IPv4 ----
TEST(Ipv4Validation, AcceptsDottedQuadOnly) {
    EXPECT_TRUE(isValidIpv4Address("192.168.0.5"));
    EXPECT_TRUE(isValidIpv4Address("0.0.0.0"));
    EXPECT_FALSE(isValidIpv4Address(""));
    EXPECT_FALSE(isValidIpv4Address("256.1.1.1"));
    EXPECT_FALSE(isValidIpv4Address("1.2.3"));
    EXPECT_FALSE(isValidIpv4Address("::1"));
    EXPECT_FALSE(isValidIpv4Address("example.com"));
}

TEST(Ipv4Validation, RejectsEmbeddedNulAfterValidAddress) {
    EXPECT_FALSE(isValidIpv4Address(std::string("192.168.0.1\0x", 13)));
    EXPECT_FALSE(isValidIpv4Address(std::string("192.168.0.1\0", 12)));
}

// ---- parseCommission ----
json validMatter() {
    return json{{"protocolType", "MATTER"}, {"deviceName", "스마트 플러그"}, {"payload", "34970112332"},
                {"ssid", "MyWiFi"}, {"password", "pass word\"'"}, {"driverTypes", json::array({1})}};
}

TEST(ParseCommission, AcceptsValidMatterRequest) {
    const auto result = parseCommission(validMatter());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->protocolType, ProtocolType::MATTER);
    EXPECT_EQ(result.value->deviceName, "스마트 플러그");
    EXPECT_EQ(result.value->password, "pass word\"'");  // 공백·따옴표는 허용
    ASSERT_EQ(result.value->driverTypes.size(), 1u);
    EXPECT_EQ(result.value->driverTypes[0], DriverType::PC_DRIVER);
}

TEST(ParseCommission, OptionalFieldsDefaultToEmpty) {
    json body{{"protocolType", "TCP_DIY"}, {"deviceName", "전등"}, {"payload", ""}};
    const auto result = parseCommission(body);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->ssid, "");
    EXPECT_EQ(result.value->password, "");
    EXPECT_TRUE(result.value->driverTypes.empty());
}

TEST(ParseCommission, RejectsNonObjectAndMissingRequiredFields) {
    EXPECT_FALSE(parseCommission(json::array()).ok());
    EXPECT_FALSE(parseCommission(json(nullptr)).ok());
    for (const char* key : {"protocolType", "deviceName", "payload"}) {
        json body = validMatter();
        body.erase(key);
        const auto result = parseCommission(body);
        ASSERT_FALSE(result.ok()) << key;
        EXPECT_EQ(result.error.code, ErrorCode::InvalidRequest);
    }
}

TEST(ParseCommission, RejectsWrongTypesAndUnknownProtocol) {
    json body = validMatter();
    body["deviceName"] = 5;
    EXPECT_FALSE(parseCommission(body).ok());
    body = validMatter();
    body["ssid"] = nullptr;
    EXPECT_FALSE(parseCommission(body).ok());
    body = validMatter();
    body["protocolType"] = "ZIGBEE";
    EXPECT_FALSE(parseCommission(body).ok());
}

std::string repeat(const std::string& unit, size_t times) {
    std::string result;
    for (size_t i = 0; i < times; ++i) result += unit;
    return result;
}

TEST(ParseCommission, DeviceNameLengthIsCountedInCharacters) {
    json body = validMatter();
    body["deviceName"] = "";
    EXPECT_FALSE(parseCommission(body).ok());
    body["deviceName"] = repeat("가", 64);  // 192바이트지만 64자
    EXPECT_TRUE(parseCommission(body).ok());
    body["deviceName"] = repeat("가", 65);
    EXPECT_FALSE(parseCommission(body).ok());
}

TEST(ParseCommission, SsidAndPasswordLengthAreCountedInBytes) {
    json body = validMatter();
    body["ssid"] = std::string(32, 'a');
    body["password"] = std::string(63, 'a');
    EXPECT_TRUE(parseCommission(body).ok());
    body["ssid"] = std::string(33, 'a');
    EXPECT_FALSE(parseCommission(body).ok());
    body["ssid"] = repeat("가", 11);  // 33바이트
    EXPECT_FALSE(parseCommission(body).ok());
    body["ssid"] = "ok";
    body["password"] = std::string(64, 'a');
    EXPECT_FALSE(parseCommission(body).ok());
}

TEST(ParseCommission, RejectsControlCharactersInStringFields) {
    for (const char* key : {"deviceName", "ssid", "password"}) {
        for (const char* bad : {"a\nb", "a\rb", "a\tb"}) {
            json body = validMatter();
            body[key] = bad;
            EXPECT_FALSE(parseCommission(body).ok()) << key;
        }
        json body = validMatter();
        body[key] = std::string("a\0b", 3);
        EXPECT_FALSE(parseCommission(body).ok()) << key << " NUL";
    }
}

TEST(ParseCommission, MatterPayloadFormats) {
    const auto accepts = [](const std::string& payload) {
        json body = validMatter();
        body["payload"] = payload;
        return parseCommission(body).ok();
    };
    EXPECT_TRUE(accepts("34970112332"));                         // 11자리
    EXPECT_TRUE(accepts("749701123365521327694"));               // 21자리
    EXPECT_TRUE(accepts("MT:Y.K9042C00KA0648G00"));
    EXPECT_FALSE(accepts("3497011233"));                         // 10자리
    EXPECT_FALSE(accepts("349701123321"));                       // 12자리
    EXPECT_FALSE(accepts("3497-011-2332"));                      // 하이픈은 앱이 제거
    EXPECT_FALSE(accepts("MT:"));
    EXPECT_FALSE(accepts("MT:abc"));                             // 소문자
    EXPECT_FALSE(accepts("MT:AB CD"));
    EXPECT_FALSE(accepts(""));
}

TEST(ParseCommission, TcpPayloadIsIpv4OrEmpty) {
    const auto accepts = [](const std::string& payload) {
        json body{{"protocolType", "TCP_DIY"}, {"deviceName", "전등"}, {"payload", payload}};
        return parseCommission(body).ok();
    };
    EXPECT_TRUE(accepts(""));
    EXPECT_TRUE(accepts("192.168.0.10"));
    EXPECT_FALSE(accepts("192.168.0"));
    EXPECT_FALSE(accepts("host.local"));
    EXPECT_FALSE(accepts("192.168.0.10\n"));
}

TEST(ParseCommission, PayloadWithControlCharactersOrNulIsRejected) {
    for (const char* protocol : {"MATTER", "TCP_DIY"}) {
        for (const std::string& bad : {std::string("34970112332\n"), std::string("192.168.0.1\0x", 13),
                                       std::string("a\rb"), std::string("\t")}) {
            json body{{"protocolType", protocol}, {"deviceName", "전등"}, {"payload", bad}};
            EXPECT_FALSE(parseCommission(body).ok()) << protocol;
        }
    }
}

TEST(ParseCommission, InvalidUtf8IsRejectedInTextFields) {
    const std::string invalid[] = {
        "\x80", "ab\xBF",                // 연속 바이트만
        "ab\xC3", "\xE2\x82", "\xF0\x9F\x98",  // 잘린 시퀀스
        "\xC0\xAF", "\xE0\x80\xAF",      // overlong
        "\xED\xA0\x80",                 // 서로게이트
        "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFF"};
    for (const char* key : {"deviceName", "ssid", "password"}) {
        for (const std::string& bad : invalid) {
            json body = validMatter();
            body[key] = bad;  // nlohmann은 dump 때만 검사하므로 직접 넣어도 객체는 만들어진다
            EXPECT_FALSE(parseCommission(body).ok()) << key;
        }
    }
    json ok = validMatter();
    ok["deviceName"] = "가나다😀";  // 4바이트 글자
    EXPECT_TRUE(parseCommission(ok).ok());
}

TEST(ParseCommission, NullOptionalFieldsAndDriverTypesAreRejected) {
    for (const char* key : {"ssid", "password", "driverTypes"}) {
        json body = validMatter();
        body[key] = nullptr;
        EXPECT_FALSE(parseCommission(body).ok()) << key;
    }
}

TEST(ParseCommission, EmptyDriverTypesArrayIsAcceptedBoolElementsAreNot) {
    json body = validMatter();
    body["driverTypes"] = json::array();
    const auto empty = parseCommission(body);
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(empty.value->driverTypes.empty());
    body["driverTypes"] = json::array({true});
    EXPECT_FALSE(parseCommission(body).ok());
    body["driverTypes"] = json::array({1, false});
    EXPECT_FALSE(parseCommission(body).ok());
}

TEST(ParseCommission, DriverTypesMustBeOneToThree) {
    json body = validMatter();
    body["driverTypes"] = json::array({1, 2, 3});
    EXPECT_TRUE(parseCommission(body).ok());
    for (const json& bad : {json::array({0}), json::array({4}), json::array({-1}), json::array({"1"}),
                            json::array({1.5}), json(1)}) {
        body["driverTypes"] = bad;
        EXPECT_FALSE(parseCommission(body).ok()) << bad.dump();
    }
}

// ---- parseHistoryQuery ----
httplib::Params params(std::initializer_list<std::pair<const std::string, std::string>> items) {
    httplib::Params result;
    for (const auto& item : items) result.insert(item);
    return result;
}

TEST(ParseHistoryQuery, EmptyParamsGiveDefaults) {
    const auto result = parseHistoryQuery(params({}));
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value->from);
    EXPECT_FALSE(result.value->period);
    EXPECT_EQ(result.value->limit, 50);
}

TEST(ParseHistoryQuery, LimitRules) {
    EXPECT_EQ(parseHistoryQuery(params({{"limit", "3"}})).value->limit, 3);
    EXPECT_EQ(parseHistoryQuery(params({{"limit", "1000"}})).value->limit, 1000);
    EXPECT_EQ(parseHistoryQuery(params({{"limit", "1001"}})).value->limit, 1000);  // 1000으로 줄임
    EXPECT_EQ(parseHistoryQuery(params({{"limit", "9223372036854775807"}})).value->limit, 1000);
    for (const char* bad : {"0", "-1", "abc", "1.5", "", " 5", "9223372036854775808", "99999999999999999999"}) {
        const auto result = parseHistoryQuery(params({{"limit", bad}}));
        ASSERT_FALSE(result.ok()) << bad;
        EXPECT_EQ(result.error.code, ErrorCode::InvalidRequest);
    }
}

TEST(ParseHistoryQuery, DateFormatAndCalendarChecks) {
    ASSERT_TRUE(parseHistoryQuery(params({{"from", "2026-11-01"}})).ok());
    EXPECT_TRUE(parseHistoryQuery(params({{"from", "2024-02-29"}})).ok());   // 윤일
    EXPECT_FALSE(parseHistoryQuery(params({{"from", "2026-02-29"}})).ok());  // 평년
    EXPECT_FALSE(parseHistoryQuery(params({{"from", "2026-02-30"}})).ok());
    EXPECT_FALSE(parseHistoryQuery(params({{"from", "2026-13-01"}})).ok());
    EXPECT_FALSE(parseHistoryQuery(params({{"to", "2026-00-10"}})).ok());
    EXPECT_FALSE(parseHistoryQuery(params({{"to", "2026-11-00"}})).ok());
    for (const char* bad : {"2026-1-01", "20261101", "2026/11/01", "2026-11-01T00:00", "", "abcd-ef-gh"}) {
        EXPECT_FALSE(parseHistoryQuery(params({{"from", bad}})).ok()) << bad;
    }
}

TEST(ParseHistoryQuery, CenturyLeapYearRules) {
    EXPECT_FALSE(parseHistoryQuery(params({{"from", "2100-02-29"}})).ok());  // 100의 배수는 평년
    EXPECT_TRUE(parseHistoryQuery(params({{"from", "2000-02-29"}})).ok());   // 400의 배수는 윤년
}

TEST(ParseHistoryQuery, DuplicateParametersAreRejected) {
    for (const char* key : {"from", "to", "period", "limit"}) {
        const auto result = parseHistoryQuery(params({{key, "1"}, {key, "1"}}));
        ASSERT_FALSE(result.ok()) << key;
        EXPECT_EQ(result.error.code, ErrorCode::InvalidRequest);
    }
}

TEST(ParseHistoryQuery, FullWidthDigitsAndNonAsciiDigitsAreRejected) {
    EXPECT_FALSE(parseHistoryQuery(params({{"limit", "１２"}})).ok());
    EXPECT_FALSE(parseHistoryQuery(params({{"from", "２０２６-11-01"}})).ok());
}

TEST(ParseHistoryQuery, PeriodValuesAndRequiresBothDates) {
    for (const char* period : {"day", "week", "month"}) {
        EXPECT_TRUE(parseHistoryQuery(params({{"period", period}, {"from", "2026-11-01"}, {"to", "2026-11-17"}})).ok());
    }
    EXPECT_FALSE(parseHistoryQuery(params({{"period", "year"}, {"from", "2026-11-01"}, {"to", "2026-11-17"}})).ok());
    EXPECT_FALSE(parseHistoryQuery(params({{"period", "day"}})).ok());
    EXPECT_FALSE(parseHistoryQuery(params({{"period", "day"}, {"from", "2026-11-01"}})).ok());
    EXPECT_FALSE(parseHistoryQuery(params({{"period", "day"}, {"to", "2026-11-01"}})).ok());
}

TEST(ParseHistoryQuery, FromAfterToIsRejectedSameDayIsAllowed) {
    EXPECT_FALSE(parseHistoryQuery(params({{"from", "2026-11-02"}, {"to", "2026-11-01"}})).ok());
    EXPECT_TRUE(parseHistoryQuery(params({{"from", "2026-11-01"}, {"to", "2026-11-01"}})).ok());
}

TEST(ParseHistoryQuery, RangeAllows366DaysButNot367) {
    // 2024는 윤년이라 1월 1일~12월 31일이 366일
    EXPECT_TRUE(parseHistoryQuery(params({{"from", "2024-01-01"}, {"to", "2024-12-31"}})).ok());
    EXPECT_FALSE(parseHistoryQuery(params({{"from", "2024-01-01"}, {"to", "2025-01-01"}})).ok());
    EXPECT_TRUE(parseHistoryQuery(params({{"from", "2026-01-01"}, {"to", "2026-12-31"}})).ok());  // 365일
    EXPECT_TRUE(parseHistoryQuery(params({{"from", "2026-01-01"}, {"to", "2027-01-01"}})).ok());  // 366일
    EXPECT_FALSE(parseHistoryQuery(params({{"from", "2026-01-01"}, {"to", "2027-01-02"}})).ok()); // 367일
}

TEST(ParseHistoryQuery, YearOutsideSupportedRangeIsRejected) {
    for (const char* bad : {"2300-01-01", "1600-01-01", "1999-12-31", "2101-01-01", "0000-01-01", "9999-12-31"}) {
        const auto asFrom = parseHistoryQuery(params({{"from", bad}}));
        ASSERT_FALSE(asFrom.ok()) << bad;
        EXPECT_EQ(asFrom.error.code, ErrorCode::InvalidRequest);
        EXPECT_FALSE(parseHistoryQuery(params({{"to", bad}})).ok()) << bad;
    }
}

TEST(ParseHistoryQuery, YearRangeBoundariesAreAccepted) {
    EXPECT_TRUE(parseHistoryQuery(params({{"from", "2000-01-01"}})).ok());
    EXPECT_TRUE(parseHistoryQuery(params({{"to", "2100-12-31"}})).ok());
}

TEST(ParseHistoryQuery, OnlyFromOrOnlyToIsFineInRawMode) {
    EXPECT_TRUE(parseHistoryQuery(params({{"from", "2020-01-01"}})).ok());
    EXPECT_TRUE(parseHistoryQuery(params({{"to", "2030-01-01"}})).ok());
}

}  // namespace

TEST(RequestValidationTest, LanIpv4AcceptsPrivateAndLinkLocalOnly) {
    for (const char* ok : {"10.0.0.1", "172.16.0.1", "172.31.255.254", "192.168.219.109", "169.254.1.2"}) {
        EXPECT_TRUE(RequestValidation::isLanIpv4Address(ok)) << ok;
    }
    for (const char* bad : {"127.0.0.1", "0.0.0.0", "8.8.8.8", "172.15.0.1", "172.32.0.1", "192.169.0.1", "",
                            "::1", "::ffff:192.168.0.1", "192.168.0"}) {
        EXPECT_FALSE(RequestValidation::isLanIpv4Address(bad)) << bad;
    }
    EXPECT_FALSE(RequestValidation::isLanIpv4Address(std::string("192.168.0.1\0x", 13)));
}

TEST(RequestValidationTest, JobIdFormatIsLowercaseDigitsAndHyphenUpTo32) {
    EXPECT_TRUE(RequestValidation::isValidJobId("c-tk3q5s-1"));
    EXPECT_TRUE(RequestValidation::isValidJobId(std::string(32, 'a')));
    EXPECT_FALSE(RequestValidation::isValidJobId(""));
    EXPECT_FALSE(RequestValidation::isValidJobId(std::string(33, 'a')));
    EXPECT_FALSE(RequestValidation::isValidJobId("C-1"));
    EXPECT_FALSE(RequestValidation::isValidJobId("c_1"));
    EXPECT_FALSE(RequestValidation::isValidJobId("c/1"));
}
