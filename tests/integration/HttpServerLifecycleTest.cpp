#include <gtest/gtest.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "CommissioningService.hpp"
#include "HTTPServer.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "support/RawHttpClient.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace testsupport;
using namespace std::chrono_literals;

namespace {

const std::string kToken = "test-token-0123456789abcdef0123456789";

// 포트를 점유하는 리슨 소켓(바인딩 실패 시험용). port 0이면 OS가 고른 포트를 잡고 port()로 알려 준다
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
        socklen_t length = sizeof(addr);
        if (held_ && ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &length) == 0) port_ = ntohs(addr.sin_port);
    }
    ~PortHolder() { release(); }
    bool held() const { return held_; }
    int port() const { return port_; }
    void release() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
    bool held_ = false;
    int port_ = 0;
};

class HttpServerLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    }

    // 새 연결로 GET /api/devices를 보내 200을 받는지(포트 0 서버는 시작할 때마다 포트가 바뀌므로 지금 포트를 읽는다)
    bool answersRequests(const HTTPServer& server) {
        RawHttpClient connection(server.boundPort(), 3);
        if (!connection.connected()) return false;
        connection.sendAll(makeGetRequest("/api/devices", kToken));
        return connection.readResponse().rfind("HTTP/1.1 200", 0) == 0;
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

TEST_F(HttpServerLifecycleTest, StopWithoutStartAndDestructorWithoutStartAreSafe) {
    HTTPServer server(manager, commissioning, kToken, port);
    server.stop();
    server.stop();
    EXPECT_FALSE(server.waitUntilReady(10ms));
}

TEST_F(HttpServerLifecycleTest, SecondStartWhileRunningReturnsFalse) {
    HTTPServer server(manager, commissioning, kToken, port);
    ASSERT_TRUE(server.start());
    EXPECT_FALSE(server.start());
    ASSERT_TRUE(server.waitUntilReady(3s));
    EXPECT_FALSE(server.start());
    EXPECT_TRUE(answersRequests(server));
}

TEST_F(HttpServerLifecycleTest, StopTwiceIsSafe) {
    HTTPServer server(manager, commissioning, kToken, port);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitUntilReady(3s));
    server.stop();
    server.stop();
}

TEST_F(HttpServerLifecycleTest, StopRightAfterStartRepeatedlyNeverHangs) {
    for (int i = 0; i < 30; ++i) {
        HTTPServer server(manager, commissioning, kToken, port);
        ASSERT_TRUE(server.start()) << i;
        server.stop();  // listen에 들어가기 전이어도 끝나야 한다
    }
}

TEST_F(HttpServerLifecycleTest, SameObjectCanStartStopStartAgainAndServes) {
    HTTPServer server(manager, commissioning, kToken, port);
    for (int round = 0; round < 3; ++round) {
        ASSERT_TRUE(server.start()) << round;
        ASSERT_TRUE(server.waitUntilReady(3s)) << round;
        EXPECT_TRUE(answersRequests(server)) << round;
        server.stop();
    }
}

TEST_F(HttpServerLifecycleTest, ConcurrentStartsLetExactlyOneWin) {
    HTTPServer server(manager, commissioning, kToken, port);
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] { if (server.start()) ++winners; });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(winners.load(), 1);
    ASSERT_TRUE(server.waitUntilReady(3s));
    EXPECT_TRUE(answersRequests(server));
}

TEST_F(HttpServerLifecycleTest, ConcurrentStartAndStopCallsEndInACleanState) {
    HTTPServer server(manager, commissioning, kToken, port);
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

TEST_F(HttpServerLifecycleTest, ConcurrentStartStopAndWaitUntilReadyAreRaceFree) {
    HTTPServer server(manager, commissioning, kToken, port);
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        while (!go) std::this_thread::yield();
        for (int i = 0; i < 20; ++i) server.start();
    });
    threads.emplace_back([&] {
        while (!go) std::this_thread::yield();
        for (int i = 0; i < 20; ++i) server.stop();
    });
    for (int w = 0; w < 2; ++w) {
        threads.emplace_back([&] {
            while (!go) std::this_thread::yield();
            while (!done) server.waitUntilReady(5ms);
        });
    }
    go = true;
    threads[0].join();
    threads[1].join();
    done = true;
    threads[2].join();
    threads[3].join();
    server.stop();
    ASSERT_TRUE(server.start());
    EXPECT_TRUE(server.waitUntilReady(3s));
}

TEST_F(HttpServerLifecycleTest, NullAuthenticatorIsRejectedAtConstruction) {
    EXPECT_THROW(HTTPServer(manager, commissioning, std::shared_ptr<const Authenticator>(), port, "127.0.0.1", clock),
                 std::invalid_argument);
}

TEST_F(HttpServerLifecycleTest, PortOutOfRangeFailsToStart) {
    for (int bad : {-1, 65536, 100000}) {
        HTTPServer server(manager, commissioning, kToken, bad);
        testing::internal::CaptureStderr();
        EXPECT_FALSE(server.start()) << bad;
        testing::internal::GetCapturedStderr();
        EXPECT_FALSE(server.waitUntilReady(10ms)) << bad;
    }
}

TEST_F(HttpServerLifecycleTest, StartLogIsPrintedOnlyAfterBindingSucceeds) {
    PortHolder holder(0);
    ASSERT_TRUE(holder.held());
    port = holder.port();
    HTTPServer server(manager, commissioning, kToken, port);
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    EXPECT_FALSE(server.start());
    EXPECT_FALSE(server.waitUntilReady(10ms));
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(testing::internal::GetCapturedStdout().find("HTTP 서버 시작"), std::string::npos);
}

TEST_F(HttpServerLifecycleTest, BindFailureLogsAddressPortAndErrnoThenRestartSucceedsAfterRelease) {
    PortHolder holder(0);
    ASSERT_TRUE(holder.held());
    port = holder.port();

    HTTPServer server(manager, commissioning, kToken, port);
    testing::internal::CaptureStderr();
    EXPECT_FALSE(server.start());
    EXPECT_FALSE(server.waitUntilReady(10ms));
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("0.0.0.0"), std::string::npos) << log;
    EXPECT_NE(log.find(std::to_string(port)), std::string::npos) << log;
    EXPECT_NE(log.find("errno=" + std::to_string(EADDRINUSE)), std::string::npos) << log;

    holder.release();
    ASSERT_TRUE(server.start());  // 바인딩 실패 뒤에도 같은 객체로 다시 시작할 수 있다
    EXPECT_TRUE(server.waitUntilReady(3s));
    EXPECT_TRUE(answersRequests(server));
}

TEST_F(HttpServerLifecycleTest, BindsOnlyToTheGivenAddress) {
    HTTPServer server(manager, commissioning, kToken, port, "127.0.0.1");
    testing::internal::CaptureStdout();
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitUntilReady(3s));
    const std::string log = testing::internal::GetCapturedStdout();
    EXPECT_NE(log.find("127.0.0.1"), std::string::npos) << log;
    EXPECT_NE(log.find("평문 HTTP"), std::string::npos) << log;
    EXPECT_TRUE(answersRequests(server));

    // 같은 포트를 다른 주소(127.0.0.2)에 열 수 있다 = 서버가 0.0.0.0이 아니라 127.0.0.1에만 열려 있다
    PortHolder other(server.boundPort(), "127.0.0.2");
    EXPECT_TRUE(other.held());
}

TEST_F(HttpServerLifecycleTest, PortZeroBindsAnOsChosenPortAndBoundPortFollowsStartStop) {
    HTTPServer server(manager, commissioning, kToken, 0);
    EXPECT_EQ(server.boundPort(), 0);  // 시작 전
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitUntilReady(3s));
    EXPECT_GT(server.boundPort(), 0);
    EXPECT_TRUE(answersRequests(server));
    server.stop();
    EXPECT_EQ(server.boundPort(), 0);  // stop 뒤
}

// main처럼 포트를 지정하면 그 포트로 열린다(포트 0 지원이 운영 동작을 바꾸지 않음)
TEST_F(HttpServerLifecycleTest, GivenPortIsUsedExactlyAsBefore) {
    int givenPort = 0;
    {
        PortHolder probe(0, "127.0.0.1");  // 빈 포트 하나를 알아낸 뒤 놓는다
        ASSERT_TRUE(probe.held());
        givenPort = probe.port();
    }
    HTTPServer server(manager, commissioning, kToken, givenPort, "127.0.0.1");
    testing::internal::CaptureStdout();
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitUntilReady(3s));
    const std::string log = testing::internal::GetCapturedStdout();
    EXPECT_EQ(server.boundPort(), givenPort);
    EXPECT_NE(log.find("포트: " + std::to_string(givenPort)), std::string::npos) << log;
    EXPECT_TRUE(answersRequests(server));
    server.stop();
}

TEST_F(HttpServerLifecycleTest, BindFailureLeavesBoundPortZero) {
    PortHolder holder(0);
    ASSERT_TRUE(holder.held());
    HTTPServer server(manager, commissioning, kToken, holder.port());
    testing::internal::CaptureStderr();
    EXPECT_FALSE(server.start());
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(server.boundPort(), 0);
}

}  // namespace
