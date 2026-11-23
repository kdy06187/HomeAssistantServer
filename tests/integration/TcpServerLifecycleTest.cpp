#include <gtest/gtest.h>
#include "TcpServer.hpp"
#include "fakes/FakeMacResolver.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <thread>
#include <vector>

namespace {

FakeMacResolver testResolver;

TEST(TcpServerLifecycle, StopWithoutStartAndTwiceIsSafe) {
    TcpServer server(testResolver);
    server.stop();
    server.stop();
}

TEST(TcpServerLifecycle, StartTwiceReturnsFalseAndKeepsOneListener) {
    TcpServer server(testResolver);
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    const int port = server.boundPort();
    EXPECT_FALSE(server.start(0, "127.0.0.1"));
    EXPECT_EQ(server.boundPort(), port);
    server.stop();
    EXPECT_EQ(server.boundPort(), 0);  // 멈춘 뒤에는 0
}

TEST(TcpServerLifecycle, PortOutOfRangeFailsToStart) {
    TcpServer server(testResolver);
    for (int bad : {-1, 65536, 100000}) {
        testing::internal::CaptureStderr();
        EXPECT_FALSE(server.start(bad, "127.0.0.1")) << bad;
        testing::internal::GetCapturedStderr();
    }
    EXPECT_EQ(server.boundPort(), 0);
}

TEST(TcpServerLifecycle, RepeatedStartStopWorks) {
    TcpServer server(testResolver);
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(server.start(0, "127.0.0.1")) << i;
        server.stop();
    }
}

TEST(TcpServerLifecycle, ConcurrentStartsLetExactlyOneWinWithOneListener) {
    TcpServer server(testResolver);
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
    TcpServer server(testResolver);
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
    TcpServer holder(testResolver);
    ASSERT_TRUE(holder.start(0, "127.0.0.1"));
    const int port = holder.boundPort();

    TcpServer second(testResolver);
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
    TcpServer server(testResolver);
    EXPECT_FALSE(server.start(0, "not-an-ip"));
    EXPECT_FALSE(server.start(0, std::string("127.0.0.1\0junk", 14))); // NUL 뒤를 무시하고 통과시키지 않는다
    EXPECT_TRUE(server.start(0, "127.0.0.1"));
    server.stop();
}

bool canConnect(const char* address, int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    inet_pton(AF_INET, address, &target.sin_addr);
    const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0;
    ::close(fd);
    return connected;
}

TEST(TcpServerLifecycle, BindsOnlyToTheGivenAddress) {
    TcpServer server(testResolver);
    testing::internal::CaptureStdout();
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    const std::string log = testing::internal::GetCapturedStdout();
    EXPECT_NE(log.find("127.0.0.1"), std::string::npos) << log;
    EXPECT_TRUE(canConnect("127.0.0.1", server.boundPort()));
    // 127.0.0.2로는 닿지 않는다 = 0.0.0.0이 아니라 127.0.0.1에만 열려 있다
    EXPECT_FALSE(canConnect("127.0.0.2", server.boundPort()));
}

TEST(TcpServerLifecycle, DefaultAddressListensOnAllInterfaces) {
    TcpServer server(testResolver);
    ASSERT_TRUE(server.start(0));
    EXPECT_TRUE(canConnect("127.0.0.2", server.boundPort()));
}

}  // namespace
