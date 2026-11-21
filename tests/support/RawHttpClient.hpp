#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <string>

// HTTP 서버 통합 테스트용 원시 소켓 도구. 요청 문자열을 그대로 보내고 응답을 직접 읽는다.
namespace testsupport {

// 운영 포트(8000·8080)와 겹치지 않도록 항상 OS가 고른 빈 포트를 쓴다
inline int findFreePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    const int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

class RawHttpClient {
public:
    RawHttpClient(int port, int receiveTimeoutSeconds) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        connected_ = connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        timeval timeout{receiveTimeoutSeconds, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    ~RawHttpClient() { close(fd_); }
    RawHttpClient(const RawHttpClient&) = delete;
    RawHttpClient& operator=(const RawHttpClient&) = delete;

    bool connected() const { return connected_; }

    void sendAll(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) { sendErrno = errno; return; }
            sent += n;
        }
    }

    // 헤더와 Content-Length만큼의 본문까지 읽는다. 끊기거나 시간이 지나면 거기까지
    std::string readResponse() {
        std::string data;
        char buffer[4096];
        size_t expected = std::string::npos;
        for (;;) {
            const size_t headerEnd = data.find("\r\n\r\n");
            if (expected == std::string::npos && headerEnd != std::string::npos) {
                expected = headerEnd + 4 + contentLength(data.substr(0, headerEnd));
            }
            if (expected != std::string::npos && data.size() >= expected) break;
            const ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
            if (n < 0) { receiveErrno = errno; break; }
            if (n == 0) { closedByPeer = true; break; }
            data.append(buffer, n);
        }
        return data;
    }

    // 서버가 연결을 닫을 때까지(또는 시간 초과까지) 기다리고 걸린 시간을 돌려준다
    std::chrono::milliseconds waitUntilClosed() {
        const auto start = std::chrono::steady_clock::now();
        char buffer[256];
        for (;;) {
            const ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
            if (n == 0) closedByPeer = true;
            if (n <= 0) break;
            if (received.empty()) firstByteAfter = elapsedSince(start);
            received.append(buffer, n);
        }
        return elapsedSince(start);
    }

    std::chrono::milliseconds firstByteAfter{-1};  // 첫 바이트가 온 시각(없으면 -1)
    std::string received;  // waitUntilClosed 동안 받은 바이트
    int receiveErrno = 0;
    int sendErrno = 0;
    bool closedByPeer = false;

private:
    static std::chrono::milliseconds elapsedSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    }
    static size_t contentLength(const std::string& header) {
        const auto pos = header.find("Content-Length: ");
        if (pos == std::string::npos) return 0;
        return std::stoul(header.substr(pos + 16));
    }
    int fd_;
    bool connected_ = false;
};

inline std::string makeGetRequest(const std::string& path, const std::string& token) {
    return "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " + token + "\r\n\r\n";
}

inline std::string makePostRequest(const std::string& path, const std::string& token, const std::string& body) {
    return "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " + token +
           "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + body;
}

}  // namespace testsupport
