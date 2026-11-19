#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// DIY 기기 소켓 하나. fd는 소멸자에서만 닫는다(RAII).
// shutdown()은 막혀 있는 송수신을 깨우기만 하므로, 다른 스레드가 이 객체를 쥐고 있어도
// 닫힌 fd 번호가 재사용되어 남의 소켓을 건드리는 일이 없다.
class TcpConnection {
public:
    static constexpr std::size_t kMaxLineBytes = 4096;

    TcpConnection(int fd, std::string peerIp);
    ~TcpConnection();
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // 요청 한 줄을 보내고 응답 줄을 받는다(같은 연결의 요청끼리는 ioMutex_로 한 쌍씩).
    // isAcceptable을 만족하는 줄이 올 때까지 timeout 안에서 기다리고, 그 시점에 이미 도착한 줄까지 함께 돌려준다.
    // 줄바꿈 없이 끝났어도 isAcceptable을 만족하면 한 줄로 본다.
    // 실패(전송 실패·끊김·한 줄 상한 초과·완성된 줄 없이 시간 초과)면 nullopt
    std::optional<std::vector<std::string>> request(const std::string& line, std::chrono::milliseconds timeout,
                                                    const std::function<bool(const std::string&)>& isAcceptable);

    // 응답을 기다리지 않고 한 줄 보낸다
    bool sendOnly(const std::string& line);

    // 이후 요청을 실패시키고 막힌 송수신을 깨운다. 여러 번 불러도 안전하다
    void shutdown();
    bool isClosed() const { return closed_.load(); }
    const std::string& peerIp() const { return peerIp_; }

private:
    bool sendAllLocked(const std::string& data);
    // 요청 전에 쌓여 있던 입력(늦게 온 이전 응답 등)을 버린다. 상대가 끊었으면 false
    bool discardPendingInputLocked();
    // buffer_에서 완성된 줄을 꺼낸다. 상한을 넘으면 false
    bool extractLinesLocked(std::vector<std::string>& outLines);

    const int fd_;
    const std::string peerIp_;
    std::mutex ioMutex_;   // 이 소켓의 송수신과 buffer_
    std::string buffer_;
    std::atomic<bool> closed_{false};
};
