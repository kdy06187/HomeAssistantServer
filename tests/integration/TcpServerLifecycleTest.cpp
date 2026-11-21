#include <gtest/gtest.h>
#include "TcpServer.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <thread>
#include <vector>

namespace {

TEST(TcpServerLifecycle, StopWithoutStartAndTwiceIsSafe) {
    TcpServer server;
    server.stop();
    server.stop();
}

TEST(TcpServerLifecycle, StartTwiceReturnsFalseAndKeepsOneListener) {
    TcpServer server;
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    const int port = server.boundPort();
    EXPECT_FALSE(server.start(0, "127.0.0.1"));
    EXPECT_EQ(server.boundPort(), port);
    server.stop();
    EXPECT_EQ(server.boundPort(), 0);  // 멈춘 뒤에는 0
}

TEST(TcpServerLifecycle, PortOutOfRangeFailsToStart) {
    TcpServer server;
    for (int bad : {-1, 65536, 100000}) {
        testing::internal::CaptureStderr();
        EXPECT_FALSE(server.start(bad, "127.0.0.1")) << bad;
        testing::internal::GetCapturedStderr();
    }
    EXPECT_EQ(server.boundPort(), 0);
}

TEST(TcpServerLifecycle, RepeatedStartStopWorks) {
    TcpServer server;
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(server.start(0, "127.0.0.1")) << i;
        server.stop();
    }
}

TEST(TcpServerLifecycle, ConcurrentStartsLetExactlyOneWinWithOneListener) {
    TcpServer server;
    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] { if (server.start(0, "127.0.0.1")) ++successes; });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(successes.load(), 1);
    EXPECT_GT(server.boundPort(), 0);
    server.stop();
}

TEST(TcpServerLifecycle, ConcurrentStartAndStopEndInACleanState) {
    TcpServer server;
    std::atomic<bool> go{false};
    std::thread starter([&] {
        while (!go) std::this_thread::yield();
        for (int i = 0; i < 50; ++i) server.start(0, "127.0.0.1");
    });
    std::thread stopper([&] {
        while (!go) std::this_thread::yield();
        for (int i = 0; i < 50; ++i) server.stop();
    });
    go = true;
    starter.join();
    stopper.join();
    server.stop();
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    server.stop();
}

TEST(TcpServerLifecycle, BindFailureLogsAddressPortAndErrno) {
    TcpServer holder;
    ASSERT_TRUE(holder.start(0, "127.0.0.1"));
    const int port = holder.boundPort();

    TcpServer second;
    testing::internal::CaptureStderr();
    EXPECT_FALSE(second.start(port, "127.0.0.1"));
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("127.0.0.1"), std::string::npos) << log;
    EXPECT_NE(log.find(std::to_string(port)), std::string::npos) << log;
    EXPECT_NE(log.find("errno=" + std::to_string(EADDRINUSE)), std::string::npos) << log;

    // 실패한 서버는 깨끗한 상태라 포트가 비면 다시 시작할 수 있다
    holder.stop();
    EXPECT_TRUE(second.start(port, "127.0.0.1"));
    second.stop();
}

TEST(TcpServerLifecycle, InvalidBindAddressFailsWithoutLeavingStateBehind) {
    TcpServer server;
    EXPECT_FALSE(server.start(0, "not-an-ip"));
    EXPECT_TRUE(server.start(0, "127.0.0.1"));
    server.stop();
}

}  // namespace
