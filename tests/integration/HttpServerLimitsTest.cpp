#include <gtest/gtest.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "HTTPServer.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "support/RawHttpClient.hpp"
#include <memory>

using namespace testsupport;

namespace {

const std::string kToken = "test-token-0123456789abcdef0123456789";

class HttpServerLimitsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
        port = findFreePort();
    }

    std::unique_ptr<HTTPServer> makeServer(int onPort) {
        return std::make_unique<HTTPServer>(manager, kToken, onPort);
    }
    std::unique_ptr<HTTPServer> startServer(int onPort) {
        auto server = makeServer(onPort);
        EXPECT_TRUE(server->start());
        return server;
    }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DeviceManager manager{database, deviceDrivers, clock};
    FakeProtocolDriver tcpFake;
    int port = 0;
};

TEST_F(HttpServerLimitsTest, KeepAliveClosesAfter20RequestsOnOneConnection) {
    auto server = startServer(port);
    ASSERT_TRUE(server->waitUntilReady(std::chrono::seconds(3)));

    RawHttpClient connection(port, 3);
    ASSERT_TRUE(connection.connected());
    for (int i = 1; i <= 19; ++i) {
        connection.sendAll(makeGetRequest("/api/devices", kToken));
        const std::string response = connection.readResponse();
        ASSERT_EQ(response.rfind("HTTP/1.1 200", 0), 0u) << "요청 " << i << ": " << response;
        EXPECT_EQ(response.find("Connection: close"), std::string::npos) << "요청 " << i;
    }
    connection.sendAll(makeGetRequest("/api/devices", kToken));
    const std::string last = connection.readResponse();
    EXPECT_EQ(last.rfind("HTTP/1.1 200", 0), 0u) << last;
    EXPECT_NE(last.find("Connection: close"), std::string::npos) << "20번째 응답은 연결을 닫는다고 알려야 함: " << last;
    server->stop();
}

TEST_F(HttpServerLimitsTest, SecondServerOnSamePortFailsToStart) {
    auto first = startServer(port);
    ASSERT_TRUE(first->waitUntilReady(std::chrono::seconds(3)));

    // SO_REUSEPORT를 끄고 SO_REUSEADDR만 쓰므로 같은 포트를 두 번째 서버가 열 수 없다
    auto second = makeServer(port);
    testing::internal::CaptureStderr();
    EXPECT_FALSE(second->start());
    testing::internal::GetCapturedStderr();
    EXPECT_FALSE(second->waitUntilReady(std::chrono::milliseconds(50)));
    second->stop();

    // 첫 서버는 영향받지 않고 계속 응답한다
    RawHttpClient connection(port, 3);
    connection.sendAll(makeGetRequest("/api/devices", kToken));
    EXPECT_EQ(connection.readResponse().rfind("HTTP/1.1 200", 0), 0u);
    first->stop();
}

TEST_F(HttpServerLimitsTest, SamePortCanBeReusedRightAfterStop) {
    {
        auto first = startServer(port);
        ASSERT_TRUE(first->waitUntilReady(std::chrono::seconds(3)));
        {
            // 서버가 먼저 닫는 연결이 TIME_WAIT를 남기게 한다
            RawHttpClient connection(port, 3);
            connection.sendAll("GET /api/devices HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nAuthorization: Bearer " + kToken + "\r\n\r\n");
            EXPECT_EQ(connection.readResponse().rfind("HTTP/1.1 200", 0), 0u);
        }
        first->stop();
    }
    auto second = startServer(port);
    EXPECT_TRUE(second->waitUntilReady(std::chrono::seconds(3)));
    second->stop();
}

}  // namespace
