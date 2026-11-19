#include "TcpConnection.hpp"
#include <cerrno>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

constexpr std::size_t kRecvChunkBytes = 1024;

void stripCarriageReturn(std::string& line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
}

}  // namespace

TcpConnection::TcpConnection(int fd, std::string peerIp) : fd_(fd), peerIp_(std::move(peerIp)) {
    // 상대가 받지 않아 송신 버퍼가 차도 요청 스레드가 무한히 막히지 않게 한다
    timeval sendTimeout{1, 0};
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
}

TcpConnection::~TcpConnection() {
    ::close(fd_);
}

void TcpConnection::shutdown() {
    if (!closed_.exchange(true)) {
        ::shutdown(fd_, SHUT_RDWR);
    }
}

bool TcpConnection::sendOnly(const std::string& line) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (closed_) return false;
    return sendAllLocked(line);
}

bool TcpConnection::sendAllLocked(const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        // MSG_NOSIGNAL: 끊긴 소켓에 써도 SIGPIPE로 프로세스가 죽지 않게 한다
        const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool TcpConnection::discardPendingInputLocked() {
    std::size_t discarded = buffer_.size();
    buffer_.clear();
    char chunk[kRecvChunkBytes];
    for (;;) {
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n > 0) {
            discarded += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) return false;  // 상대가 연결을 닫았다
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return false;
    }
    if (discarded > 0) {
        std::cerr << "[TcpConnection] 요청 전 남아 있던 입력 " << discarded << "바이트 버림 (" << peerIp_ << ")" << std::endl;
    }
    return true;
}

bool TcpConnection::extractLinesLocked(std::vector<std::string>& outLines) {
    std::size_t start = 0;
    for (;;) {
        const std::size_t newline = buffer_.find('\n', start);
        if (newline == std::string::npos) break;
        std::string line = buffer_.substr(start, newline - start);
        start = newline + 1;
        if (line.size() > kMaxLineBytes) return false;
        stripCarriageReturn(line);
        if (!line.empty()) outLines.push_back(std::move(line));
    }
    buffer_.erase(0, start);
    return buffer_.size() <= kMaxLineBytes;
}

std::optional<std::vector<std::string>> TcpConnection::request(
        const std::string& line, std::chrono::milliseconds timeout,
        const std::function<bool(const std::string&)>& isAcceptable) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (closed_) return std::nullopt;
    if (!discardPendingInputLocked() || !sendAllLocked(line)) {
        shutdown();
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<std::string> lines;
    bool gotAcceptable = false;
    char chunk[kRecvChunkBytes];

    auto failOverflow = [this]() -> std::optional<std::vector<std::string>> {
        std::cerr << "[TcpConnection] 한 줄 상한(" << kMaxLineBytes << "바이트) 초과, 연결을 닫습니다 (" << peerIp_ << ")" << std::endl;
        shutdown();
        return std::nullopt;
    };

    for (;;) {
        const std::size_t before = lines.size();
        if (!extractLinesLocked(lines)) return failOverflow();
        for (std::size_t i = before; i < lines.size() && !gotAcceptable; ++i) gotAcceptable = isAcceptable(lines[i]);

        // 줄바꿈 없이 보내는 펌웨어도 받아들이도록, 남은 조각이 그 자체로 유효하면 한 줄로 본다
        if (!gotAcceptable && !buffer_.empty()) {
            std::string partial = buffer_;
            stripCarriageReturn(partial);
            if (isAcceptable(partial)) {
                lines.push_back(std::move(partial));
                buffer_.clear();
                gotAcceptable = true;
            }
        }

        if (gotAcceptable) {
            // 이미 도착한 나머지 줄만 더 읽는다(기다리지 않는다)
            for (;;) {
                const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), MSG_DONTWAIT);
                if (n <= 0) break;
                buffer_.append(chunk, static_cast<std::size_t>(n));
            }
            if (!extractLinesLocked(lines)) return failOverflow();
            break;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;

        pollfd waitFor{fd_, POLLIN, 0};
        const int ready = ::poll(&waitFor, 1, static_cast<int>(remaining.count()));
        if (ready < 0) {
            if (errno == EINTR) continue;
            shutdown();
            return std::nullopt;
        }
        if (ready == 0) continue;  // 다음 반복에서 마감 시각을 확인한다

        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        // 끊김(0) 또는 오류: 받은 줄이 있으면 그것으로 판단하고, 연결은 닫는다
        shutdown();
        break;
    }

    if (lines.empty()) {
        shutdown();
        return std::nullopt;
    }
    return lines;
}
