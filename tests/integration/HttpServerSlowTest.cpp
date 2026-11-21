#include <gtest/gtest.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "HTTPServer.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "support/RawHttpClient.hpp"
#include <memory>
#include <thread>
#include <vector>

using namespace testsupport;
using namespace std::chrono_literals;

// 몇 초씩 걸리는 HTTP 서버 시험. ctest 라벨 slow
namespace {

const std::string kToken = "test-token-0123456789abcdef0123456789";

class HttpServerSlowTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
        manager.addDevice("DEV_1", "시험 기기", ProtocolType::TCP_DIY, {}, &tcpFake);
        port = findFreePort();
        server = std::make_unique<HTTPServer>(manager, kToken, port);
        ASSERT_TRUE(server->start());
        ASSERT_TRUE(server->waitUntilReady(3s));
    }
    void TearDown() override { server->stop(); }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DeviceManager manager{database, deviceDrivers, clock};
    FakeProtocolDriver tcpFake;
    std::unique_ptr<HTTPServer> server;
    int port = 0;
};

// 핸들러가 7초 걸려도(타임아웃 5초보다 길다) 응답이 오고, 같은 연결을 계속 쓸 수 있어야 한다.
TEST_F(HttpServerSlowTest, HandlerSlowerThanSocketTimeoutStillAnswersAndKeepsConnection) {
    tcpFake.onReadState = [](const std::string&) { std::this_thread::sleep_for(7s); };
    tcpFake.stateToReturn = "ON";

    RawHttpClient connection(port, 30);
    ASSERT_TRUE(connection.connected());

    const auto start = std::chrono::steady_clock::now();
    connection.sendAll(makeGetRequest("/api/devices/DEV_1/state?isManualRefresh=true", kToken));
    const std::string first = connection.readResponse();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(first.rfind("HTTP/1.1 200", 0), 0u) << first;
    EXPECT_NE(first.find("\"state\":\"ON\""), std::string::npos) << first;
    EXPECT_GE(elapsed, 7s);

    tcpFake.onReadState = nullptr;
    connection.sendAll(makeGetRequest("/api/devices", kToken));
    const std::string second = connection.readResponse();
    EXPECT_EQ(second.rfind("HTTP/1.1 200", 0), 0u) << "같은 연결의 두 번째 요청: " << second;
    EXPECT_FALSE(connection.closedByPeer);
}

// 헤더 일부만 보내고 멈추면 읽기 타임아웃(5초)에 400 + Connection: close 응답이 오고,
// httplib가 소켓을 실제로 닫는 것은 그 뒤 keep-alive 대기(5초)가 끝난 약 10초 시점이다.
TEST_F(HttpServerSlowTest, StalledPartialHeaderConnectionIsAnsweredAtFiveSecondsAndClosedByTenSeconds) {
    RawHttpClient connection(port, 15);
    ASSERT_TRUE(connection.connected());
    connection.sendAll("GET /api/devices HTTP/1.1\r\nHost: local");

    const auto closedAfter = connection.waitUntilClosed();
    EXPECT_TRUE(connection.closedByPeer);
    EXPECT_EQ(connection.received.rfind("HTTP/1.1 400", 0), 0u) << connection.received;
    EXPECT_NE(connection.received.find("Connection: close"), std::string::npos);
    EXPECT_GE(connection.firstByteAfter, 4s);
    EXPECT_LE(connection.firstByteAfter, 7s);
    EXPECT_LE(closedAfter, 12s);
}

// 풀 8개 중 7개가 느린 연결에 묶여도 나머지 1개로 정상 요청이 처리된다.
TEST_F(HttpServerSlowTest, SevenStalledConnectionsDoNotBlockANormalRequest) {
    std::vector<std::unique_ptr<RawHttpClient>> stalled;
    for (int i = 0; i < 7; ++i) {
        stalled.push_back(std::make_unique<RawHttpClient>(port, 15));
        ASSERT_TRUE(stalled.back()->connected());
        stalled.back()->sendAll("GET /api/devices HTTP/1.1\r\nHost: local");
    }
    std::this_thread::sleep_for(300ms);  // 7개가 모두 작업 스레드에 잡힐 시간

    RawHttpClient normal(port, 3);
    ASSERT_TRUE(normal.connected());
    const auto start = std::chrono::steady_clock::now();
    normal.sendAll(makeGetRequest("/api/devices", kToken));
    const std::string response = normal.readResponse();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(response.rfind("HTTP/1.1 200", 0), 0u) << response;
    EXPECT_LT(elapsed, 2s);

    // 느린 연결은 약 5초 뒤 모두 닫힌다
    for (auto& connection : stalled) {
        connection->waitUntilClosed();
        EXPECT_TRUE(connection->closedByPeer);
    }
}

}  // namespace
