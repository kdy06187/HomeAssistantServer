#include "TcpConnection.hpp"
#include "json.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

constexpr std::size_t kRecvChunkBytes = 1024;
// 버린 입력의 진단용 표본 크기. 기기 응답 뒤에 남은 바이트라 비밀값은 아니지만 짧게 자른다
constexpr std::size_t kDiscardSampleBytes = 16;

// 프로세스 전체에서 처음 버린 입력 한 번만 16진으로 남긴다(무엇이 남는지 실기에서 확인하려고)
std::atomic<bool> discardDiagnosticLogged{false};

bool isWhitespaceOnly(const char* data, std::size_t size) {
    return std::all_of(data, data + size, [](char c) { return c == '\r' || c == '\n' || c == ' ' || c == '\t'; });
}

std::string toHexBytes(const std::string& bytes) {
    std::string hex;
    char pair[4];
    for (unsigned char byte : bytes) {
        std::snprintf(pair, sizeof(pair), hex.empty() ? "%02x" : " %02x", byte);
        hex += pair;
    }
    return hex;
}

void stripCarriageReturn(std::string& line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
}

// 접속 직후 기기가 보내는 인사 줄: 개행으로 끝나는 JSON 객체 한 줄(+공백류)뿐인가.
// 줄 안의 deviceId 등은 믿지 않는다(기기 ID는 MAC으로 정한다)
bool isSingleJsonObjectLine(const std::string& data) {
    const std::size_t newline = data.find('\n');
    if (newline == std::string::npos) return false;
    if (!isWhitespaceOnly(data.data() + newline, data.size() - newline)) return false;
    std::string line = data.substr(0, newline);
    stripCarriageReturn(line);
    const auto parsed = nlohmann::json::parse(line, nullptr, false);
    return !parsed.is_discarded() && parsed.is_object();
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

bool TcpConnection::isPeerGone() const {
    if (closed_) return true;
    pollfd probe{fd_, POLLRDHUP, 0};
    if (::poll(&probe, 1, 0) <= 0) return false;
    return (probe.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0;
}

bool TcpConnection::discardPendingInputLocked() {
    std::size_t discarded = buffer_.size();
    // 펌웨어가 응답 뒤에 늦게 보내는 줄바꿈("\r\n")은 해롭지 않으므로 공백류뿐이면 로그를 남기지 않는다
    bool whitespaceOnly = isWhitespaceOnly(buffer_.data(), buffer_.size());
    std::string sample = buffer_.substr(0, kDiscardSampleBytes);
    // 첫 요청이면 인사 줄인지 보려고 한 줄 상한까지만 모아 둔다(넘으면 인사 줄이 아니다)
    const bool isFirstRequest = firstRequestPending_;
    firstRequestPending_ = false;
    std::string firstDiscard = isFirstRequest ? buffer_ : std::string();
    buffer_.clear();
    const auto deadline = std::chrono::steady_clock::now() + kMaxDiscardTime;
    char chunk[kRecvChunkBytes];
    for (;;) {
        // 끝없이 보내는 기기면 여기서 영원히 돌 수 있으므로 양과 시간을 제한한다
        if (discarded > kMaxDiscardBytes || std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "[TcpConnection] 요청 전 입력이 멈추지 않음(" << discarded << "바이트), 연결을 닫습니다 (" << peerIp_ << ")" << std::endl;
            return false;
        }
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n > 0) {
            const std::size_t received = static_cast<std::size_t>(n);
            discarded += received;
            whitespaceOnly = whitespaceOnly && isWhitespaceOnly(chunk, received);
            if (isFirstRequest && firstDiscard.size() <= kMaxLineBytes) firstDiscard.append(chunk, received);
            if (sample.size() < kDiscardSampleBytes) {
                sample.append(chunk, std::min(received, kDiscardSampleBytes - sample.size()));
            }
            continue;
        }
        if (n == 0) return false;  // 상대가 연결을 닫았다
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return false;
    }
    if (discarded == 0) return true;
    // 인사 줄은 원인이 알려진 정상 입력이라 1회 진단 슬롯을 쓰지 않는다(진짜 잔여가 진단되게)
    if (isFirstRequest && firstDiscard.size() <= kMaxLineBytes && isSingleJsonObjectLine(firstDiscard)) {
        std::cout << "[TcpConnection] 접속 인사 줄 " << discarded << "바이트 처리 (" << peerIp_ << ")" << std::endl;
        return true;
    }
    if (!discardDiagnosticLogged.exchange(true)) {
        std::cerr << "[TcpConnection] 버린 잔여(1회 진단): " << toHexBytes(sample) << " (" << peerIp_ << ")" << std::endl;
    }
    if (whitespaceOnly) return true;
    std::cerr << "[TcpConnection] 요청 전 남아 있던 입력 " << discarded << "바이트 버림 (" << peerIp_ << ")" << std::endl;
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
        if (outLines.size() > kMaxLinesPerRequest) return false;
    }
    buffer_.erase(0, start);
    return buffer_.size() <= kMaxLineBytes;
}

bool TcpConnection::drainAvailableLocked(std::chrono::steady_clock::time_point deadline) {
    std::size_t drained = 0;
    char chunk[kRecvChunkBytes];
    for (;;) {
        if (drained >= kMaxDrainBytes) {
            std::cerr << "[TcpConnection] 응답 뒤 입력이 계속됨(" << drained << "바이트), 연결을 닫습니다 (" << peerIp_ << ")" << std::endl;
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) return true;
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(n));
            drained += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) return false;  // 응답 직후 끊김: 받은 줄로는 판단하고 연결은 닫는다
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
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
        std::cerr << "[TcpConnection] 응답 상한(한 줄 " << kMaxLineBytes << "바이트, " << kMaxLinesPerRequest
                  << "줄) 초과, 연결을 닫습니다 (" << peerIp_ << ")" << std::endl;
        shutdown();
        return std::nullopt;
    };
    // 줄바꿈 없이 보내는 펌웨어도 받아들이도록, 남은 조각이 그 자체로 유효하면 한 줄로 본다
    auto takePartialIfAcceptable = [this, &lines, &isAcceptable]() {
        if (buffer_.empty()) return false;
        std::string partial = buffer_;
        stripCarriageReturn(partial);
        if (!isAcceptable(partial)) return false;
        lines.push_back(std::move(partial));
        buffer_.clear();
        return true;
    };

    for (;;) {
        const std::size_t before = lines.size();
        if (!extractLinesLocked(lines)) return failOverflow();
        for (std::size_t i = before; i < lines.size() && !gotAcceptable; ++i) gotAcceptable = isAcceptable(lines[i]);
        if (!gotAcceptable) gotAcceptable = takePartialIfAcceptable();

        if (gotAcceptable) {
            // 이미 도착한 나머지만 더 읽는다(상한·마감 시각 안에서, 새로 기다리지 않는다)
            bool stillOpen = drainAvailableLocked(deadline);
            if (extractLinesLocked(lines)) {
                takePartialIfAcceptable();  // 드레인 뒤 개행 없는 마지막 조각도 마지막 줄 후보
            } else {
                // 유효 응답은 이미 받았다. 뒤따른 입력이 상한을 넘었으면 연결만 닫고 받은 줄로 판단한다
                std::cerr << "[TcpConnection] 응답 뒤 입력이 상한 초과, 연결을 닫습니다 (" << peerIp_ << ")" << std::endl;
                buffer_.clear();
                stillOpen = false;
            }
            if (!stillOpen) shutdown();
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

        // poll이 준비됐다고 해도 읽을 것이 없을 수 있어(가짜 깨움) 막히지 않게 읽는다
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        // 끊김(0) 또는 오류: 받은 줄(개행 없는 마지막 조각 포함)이 있으면 그것으로 판단하고, 연결은 닫는다
        if (!extractLinesLocked(lines)) return failOverflow();
        takePartialIfAcceptable();
        shutdown();
        break;
    }

    if (lines.empty()) {
        shutdown();
        return std::nullopt;
    }
    return lines;
}
