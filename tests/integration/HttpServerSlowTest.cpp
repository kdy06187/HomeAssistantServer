#include <gtest/gtest.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "CommissioningService.hpp"
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
        manager.addDevice("DEV_1", "시험 기기", ProtocolType::TCP_DIY, {});
        server = std::make_unique<HTTPServer>(manager, commissioning, kToken, 0);
        ASSERT_TRUE(server->start());
        port = server->boundPort();  // 포트 0: OS가 고른 포트(빈 포트를 골라 닫고 다시 여는 틈이 없다)
        ASSERT_TRUE(server->waitUntilReady(3s));
    }
    void TearDown() override { server->stop(); }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    CommissioningService commissioning{manager, {}, clock};
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

// stop()은 진행 중인 핸들러가 끝날 때까지 기다리고, 그 요청은 정상 응답을 받는다.
TEST_F(HttpServerSlowTest, StopWaitsForRunningHandlerToFinish) {
    tcpFake.onReadState = [](const std::string&) { std::this_thread::sleep_for(2s); };
    tcpFake.stateToReturn = "ON";
    RawHttpClient connection(port, 30);
    ASSERT_TRUE(connection.connected());
    connection.sendAll(makeGetRequest("/api/devices/DEV_1/state?isManualRefresh=true", kToken));
    std::this_thread::sleep_for(300ms);  // 핸들러가 시작할 시간

    const auto stopStart = std::chrono::steady_clock::now();
    server->stop();
    const auto stopElapsed = std::chrono::steady_clock::now() - stopStart;
    const std::string response = connection.readResponse();

    EXPECT_GE(stopElapsed, 1s);
    EXPECT_EQ(response.rfind("HTTP/1.1 200", 0), 0u) << response;
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

// 헤더는 다 보내고 본문을 일부만 보낸 뒤 멈추면 읽기 타임아웃(5초)에 걸려 핸들러까지 가지 못하고 연결이 닫힌다.
TEST_F(HttpServerSlowTest, StalledPostBodyTimesOutWithoutRunningTheHandler) {
    RawHttpClient connection(port, 15);
    ASSERT_TRUE(connection.connected());
    const std::string body = R"({"deviceId": "DEV_1", "command": "OFF"})";
    connection.sendAll("POST /api/control HTTP/1.1\r\nHost: local\r\nAuthorization: Bearer " + kToken +
                       "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\n\r\n" + body.substr(0, 10));

    const auto closedAfter = connection.waitUntilClosed();
    EXPECT_TRUE(connection.closedByPeer);
    EXPECT_LE(closedAfter, 12s);
    EXPECT_GE(closedAfter, 4s);  // 5초 타임아웃 전에 끊기지 않는다
    EXPECT_EQ(connection.received.find("200 OK"), std::string::npos) << connection.received;
    EXPECT_TRUE(tcpFake.sentCommands.empty()) << "본문이 끝나지 않은 요청이 실행되면 안 됨";
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

    // 느린 연결은 약 10초 뒤 모두 닫힌다(읽기 타임아웃 5초 + keep-alive 대기 5초)
    for (auto& connection : stalled) {
        connection->waitUntilClosed();
        EXPECT_TRUE(connection->closedByPeer);
    }
}

}  // namespace
