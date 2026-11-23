#pragma once
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include "fakes/FakeMacResolver.hpp"

namespace fake_tcp {
using namespace std::chrono_literals;

// 테스트가 흉내 내는 DIY 기기. 127.0.0.N에 bind해 접속하므로 기기마다 IP가 다르고,
// FakeMacResolver 기본 규칙으로 ID(TCP_0200000000NN)도 다르다
class FakeDevice {
public:
    FakeDevice(int serverPort, int lastOctet) : ip_("127.0.0." + std::to_string(lastOctet)), lastOctet_(lastOctet) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        // 닫을 때 RST로 끝내 TIME_WAIT를 남기지 않는다. 남기면 127.0.0.N의 임시 포트가 한동안 묶여,
        // 0.0.0.0에 바인드하는 다른 테스트(HTTP 서버)가 같은 포트를 받았을 때 바인드에 실패할 수 있다
        linger noLinger{1, 0};
        setsockopt(fd_, SOL_SOCKET, SO_LINGER, &noLinger, sizeof(noLinger));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        inet_pton(AF_INET, ip_.c_str(), &local.sin_addr);
        EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)), 0);
        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(static_cast<uint16_t>(serverPort));
        inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);
        EXPECT_EQ(::connect(fd_, reinterpret_cast<sockaddr*>(&server), sizeof(server)), 0);
    }
    ~FakeDevice() { closeSocket(); }

    std::string id() const { return FakeMacResolver::loopbackId(lastOctet_); }
    const std::string& ip() const { return ip_; }
    void send(const std::string& data) { ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL); }
    // 정상 종료(FIN)만 보낸다. 서버는 poll의 POLLRDHUP로 끊김을 본다(closeSocket은 RST)
    void closeGracefully() { if (fd_ >= 0) ::shutdown(fd_, SHUT_WR); }
    void closeSocket() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    // 줄 하나를 읽는다. 시간 안에 못 읽거나 끊기면 빈 문자열
    std::string readLine(std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto newline = pending_.find('\n');
            if (newline != std::string::npos) {
                std::string line = pending_.substr(0, newline + 1);
                pending_.erase(0, newline + 1);
                return line;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) return "";
            pollfd waitFor{fd_, POLLIN, 0};
            if (::poll(&waitFor, 1, static_cast<int>(left.count())) <= 0) return "";
            char chunk[1024];
            const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n <= 0) return "";
            pending_.append(chunk, static_cast<std::size_t>(n));
        }
    }

    // 서버가 이 연결을 닫았는지(EOF) 확인한다
    bool waitClosedByServer(std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) return false;
            pollfd waitFor{fd_, POLLIN, 0};
            if (::poll(&waitFor, 1, static_cast<int>(left.count())) <= 0) return false;
            char chunk[1024];
            const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n <= 0) return true;
        }
    }

    // 요청마다 respond(요청 줄)를 보내는 응답 스레드
    void serveInBackground(std::function<std::string(const std::string&)> respond) {
        stopServing_ = false;
        server_ = std::thread([this, respond] {
            while (!stopServing_) {
                const std::string request = readLine(100ms);
                if (request.empty()) continue;
                send(respond(request));
            }
        });
    }
    void stopServing() {
        stopServing_ = true;
        if (server_.joinable()) server_.join();
    }

private:
    std::string ip_;
    int lastOctet_;
    int fd_ = -1;
    std::string pending_;
    std::thread server_;
    std::atomic<bool> stopServing_{false};
};

}  // namespace fake_tcp
