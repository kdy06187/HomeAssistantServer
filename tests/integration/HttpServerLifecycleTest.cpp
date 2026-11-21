#include <gtest/gtest.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "HTTPServer.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "support/RawHttpClient.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace testsupport;
using namespace std::chrono_literals;

namespace {

const std::string kToken = "test-token-0123456789abcdef0123456789";

// 포트를 점유하는 리슨 소켓(바인딩 실패 시험용)
class PortHolder {
public:
    explicit PortHolder(int port, const char* address = "0.0.0.0") {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, address, &addr.sin_addr);
        held_ = ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && ::listen(fd_, 1) == 0;
    }
    ~PortHolder() { release(); }
    bool held() const { return held_; }
    void release() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
    bool held_ = false;
};

class HttpServerLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
        port = findFreePort();
    }

    // 새 연결로 GET /api/devices를 보내 200을 받는지
    bool answersRequests() {
        RawHttpClient connection(port, 3);
        if (!connection.connected()) return false;
        connection.sendAll(makeGetRequest("/api/devices", kToken));
        return connection.readResponse().rfind("HTTP/1.1 200", 0) == 0;
    }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DeviceManager manager{database, deviceDrivers, clock};
    FakeProtocolDriver tcpFake;
    int port = 0;
};

TEST_F(HttpServerLifecycleTest, StopWithoutStartAndDestructorWithoutStartAreSafe) {
    HTTPServer server(manager, kToken, port);
    server.stop();
    server.stop();
    EXPECT_FALSE(server.waitUntilReady(10ms));
}

TEST_F(HttpServerLifecycleTest, SecondStartWhileRunningReturnsFalse) {
    HTTPServer server(manager, kToken, port);
    ASSERT_TRUE(server.start());
    EXPECT_FALSE(server.start());
    ASSERT_TRUE(server.waitUntilReady(3s));
    EXPECT_FALSE(server.start());
    EXPECT_TRUE(answersRequests());
}

TEST_F(HttpServerLifecycleTest, StopTwiceIsSafe) {
    HTTPServer server(manager, kToken, port);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitUntilReady(3s));
    server.stop();
    server.stop();
}

TEST_F(HttpServerLifecycleTest, StopRightAfterStartRepeatedlyNeverHangs) {
    for (int i = 0; i < 30; ++i) {
        HTTPServer server(manager, kToken, port);
        ASSERT_TRUE(server.start()) << i;
        server.stop();  // listen에 들어가기 전이어도 끝나야 한다
    }
}

TEST_F(HttpServerLifecycleTest, SameObjectCanStartStopStartAgainAndServes) {
    HTTPServer server(manager, kToken, port);
    for (int round = 0; round < 3; ++round) {
        ASSERT_TRUE(server.start()) << round;
        ASSERT_TRUE(server.waitUntilReady(3s)) << round;
        EXPECT_TRUE(answersRequests()) << round;
        server.stop();
    }
}

TEST_F(HttpServerLifecycleTest, ConcurrentStartsLetExactlyOneWin) {
    HTTPServer server(manager, kToken, port);
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] { if (server.start()) ++winners; });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(winners.load(), 1);
    ASSERT_TRUE(server.waitUntilReady(3s));
    EXPECT_TRUE(answersRequests());
}

TEST_F(HttpServerLifecycleTest, ConcurrentStartAndStopCallsEndInACleanState) {
    HTTPServer server(manager, kToken, port);
    std::atomic<bool> go{false};
    std::thread starter([&] {
        while (!go) std::this_thread::yield();
        for (int i = 0; i < 20; ++i) server.start();
    });
    std::thread stopper([&] {
        while (!go) std::this_thread::yield();
        for (int i = 0; i < 20; ++i) server.stop();
    });
    go = true;
    starter.join();
    stopper.join();
    server.stop();
    // 어떤 순서로 섞였어도 정리된 뒤에는 다시 시작할 수 있다
    ASSERT_TRUE(server.start());
    EXPECT_TRUE(server.waitUntilReady(3s));
}

TEST_F(HttpServerLifecycleTest, BindFailureLogsAddressPortAndErrnoThenRestartSucceedsAfterRelease) {
    PortHolder holder(port);
    ASSERT_TRUE(holder.held());

    HTTPServer server(manager, kToken, port);
    testing::internal::CaptureStderr();
    ASSERT_TRUE(server.start());
    EXPECT_FALSE(server.waitUntilReady(2s));
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("0.0.0.0"), std::string::npos) << log;
    EXPECT_NE(log.find(std::to_string(port)), std::string::npos) << log;
    EXPECT_NE(log.find("errno=98"), std::string::npos) << log;  // EADDRINUSE

    holder.release();
    ASSERT_TRUE(server.start());  // 실패로 끝난 실행을 정리하고 다시 시작
    EXPECT_TRUE(server.waitUntilReady(3s));
    EXPECT_TRUE(answersRequests());
}

TEST_F(HttpServerLifecycleTest, BindsOnlyToTheGivenAddress) {
    HTTPServer server(manager, kToken, port, "127.0.0.1");
    testing::internal::CaptureStdout();
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitUntilReady(3s));
    const std::string log = testing::internal::GetCapturedStdout();
    EXPECT_NE(log.find("127.0.0.1"), std::string::npos) << log;
    EXPECT_NE(log.find("평문 HTTP"), std::string::npos) << log;
    EXPECT_TRUE(answersRequests());

    // 같은 포트를 다른 주소(127.0.0.2)에 열 수 있다 = 서버가 0.0.0.0이 아니라 127.0.0.1에만 열려 있다
    PortHolder other(port, "127.0.0.2");
    EXPECT_TRUE(other.held());
}

}  // namespace
