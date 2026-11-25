#include <gtest/gtest.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "CommissioningService.hpp"
#include "HTTPServer.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "support/RawHttpClient.hpp"

using namespace testsupport;
using namespace std::chrono_literals;

namespace {

const std::string kToken = "test-token-0123456789abcdef0123456789";

size_t countOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) ++count;
    return count;
}

class HttpServerAuthTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    }

    std::string statusLineFor(const std::string& request) {
        RawHttpClient connection(port, 3);
        connection.sendAll(request);
        const std::string response = connection.readResponse();
        return response.substr(0, response.find("\r\n"));
    }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    CommissioningService commissioning{manager, {}, clock};
    FakeProtocolDriver tcpFake;
    int port = 0;
};

TEST_F(HttpServerAuthTest, EmptyServerTokenRejectsEvenAnEmptyBearer) {
    HTTPServer server(manager, commissioning, std::string(), port);
    ASSERT_TRUE(server.start());
    port = server.boundPort();  // 포트 0(OS가 고름)으로 열었다
    ASSERT_TRUE(server.waitUntilReady(3s));
    EXPECT_EQ(statusLineFor(makeGetRequest("/api/devices", "")), "HTTP/1.1 401 Unauthorized");
    EXPECT_EQ(statusLineFor("GET /api/devices HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 401 Unauthorized");
    server.stop();
}

class AllowNothing final : public Authenticator {
public:
    bool isAuthorized(const std::string&) const override { return false; }
};

TEST_F(HttpServerAuthTest, InjectedAuthenticatorDecidesAccess) {
    HTTPServer server(manager, commissioning, std::make_shared<AllowNothing>(), port, "127.0.0.1", clock);
    ASSERT_TRUE(server.start());
    port = server.boundPort();  // 포트 0(OS가 고름)으로 열었다
    ASSERT_TRUE(server.waitUntilReady(3s));
    EXPECT_EQ(statusLineFor(makeGetRequest("/api/devices", kToken)), "HTTP/1.1 401 Unauthorized");
    server.stop();
}

TEST_F(HttpServerAuthTest, FailureLogHasClientMethodPathButNeverTheTokenAndSanitizesInput) {
    HTTPServer server(manager, commissioning, kToken, port);
    ASSERT_TRUE(server.start());
    port = server.boundPort();  // 포트 0(OS가 고름)으로 열었다
    ASSERT_TRUE(server.waitUntilReady(3s));

    const std::string wrongToken = "wrong-secret-token-ABCDEF";
    testing::internal::CaptureStderr();
    EXPECT_EQ(statusLineFor(makeGetRequest("/api/devices", wrongToken)), "HTTP/1.1 401 Unauthorized");
    // %01 %1B는 디코딩되어 경로에 제어문자로 들어온다
    statusLineFor(makeGetRequest("/api/x%01injected%1B", wrongToken));
    const std::string log = testing::internal::GetCapturedStderr();

    EXPECT_NE(log.find("[HTTPServer] 인증 실패: 127.0.0.1 GET /api/devices\n"), std::string::npos) << log;
    EXPECT_EQ(log.find(wrongToken), std::string::npos) << "로그에 제시된 토큰이 있으면 안 됨";
    EXPECT_EQ(log.find(kToken), std::string::npos);
    EXPECT_EQ(log.find(wrongToken.substr(0, 8)), std::string::npos) << "토큰 일부도 안 됨";
    EXPECT_NE(log.find("/api/x?injected?"), std::string::npos) << log;  // 제어문자는 ?로
    EXPECT_EQ(countOccurrences(log, "인증 실패:"), 2u) << "제어문자로 로그 줄이 갈라지면 안 됨";
    server.stop();
}

TEST_F(HttpServerAuthTest, ForwardedForHeaderIsIgnoredInFailureLog) {
    HTTPServer server(manager, commissioning, kToken, port);
    ASSERT_TRUE(server.start());
    port = server.boundPort();  // 포트 0(OS가 고름)으로 열었다
    ASSERT_TRUE(server.waitUntilReady(3s));

    testing::internal::CaptureStderr();
    EXPECT_EQ(statusLineFor("GET /api/devices HTTP/1.1\r\nHost: x\r\nX-Forwarded-For: 10.9.9.9\r\n"
                            "Forwarded: for=10.8.8.8\r\nAuthorization: Bearer bad\r\n\r\n"),
              "HTTP/1.1 401 Unauthorized");
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("인증 실패: 127.0.0.1 GET"), std::string::npos) << log;
    EXPECT_EQ(log.find("10.9.9.9"), std::string::npos) << log;
    EXPECT_EQ(log.find("10.8.8.8"), std::string::npos) << log;
    server.stop();
}

TEST_F(HttpServerAuthTest, WarnsExactlyOnceAfterMoreThanTenFailuresInAMinute) {
    HTTPServer server(manager, commissioning, std::make_shared<BearerTokenAuthenticator>(kToken), port, "127.0.0.1", clock);
    ASSERT_TRUE(server.start());
    port = server.boundPort();  // 포트 0(OS가 고름)으로 열었다
    ASSERT_TRUE(server.waitUntilReady(3s));

    testing::internal::CaptureStderr();
    for (int i = 0; i < 10; ++i) statusLineFor(makeGetRequest("/api/devices", "bad"));
    const std::string before = testing::internal::GetCapturedStderr();
    EXPECT_EQ(countOccurrences(before, "1분에"), 0u) << before;

    testing::internal::CaptureStderr();
    for (int i = 0; i < 3; ++i) statusLineFor(makeGetRequest("/api/devices", "bad"));
    const std::string after = testing::internal::GetCapturedStderr();
    EXPECT_EQ(countOccurrences(after, "1분에"), 1u) << after;

    // 정상 요청은 계속 통과한다(차단 없음)
    EXPECT_EQ(statusLineFor(makeGetRequest("/api/devices", kToken)), "HTTP/1.1 200 OK");
    server.stop();
}

}  // namespace
