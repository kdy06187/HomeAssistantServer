#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "TcpConnection.hpp"
#include "../src/json.hpp"

// DIY 기기가 접속하는 TCP 서버: 수락 스레드, 기기 ID → 연결 맵, 요청·응답.
// 락: connectionsMutex_(맵, 메모리만)와 연결별 ioMutex_(그 소켓 송수신)를 동시에 쥐지 않는다.
// 맵에서 shared_ptr을 복사한 뒤 맵 락을 풀고 연결을 쓴다.
class TcpServer {
public:
    static constexpr std::size_t kMaxConnections = 32;
    static constexpr std::chrono::milliseconds kResponseTimeout{1000};

    TcpServer() = default;
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // 바인드·리슨까지 마치고 수락 스레드를 띄운다. port 0이면 임의 포트(boundPort로 확인)
    bool start(int port, const std::string& bindAddress = "0.0.0.0");
    // 수락을 멈추고(join) 모든 연결을 닫는다. 여러 번 불러도 안전하다
    void stop();
    int boundPort() const { return boundPort_; }

    // 요청 JSON 한 줄을 보내고, 받은 줄 중 마지막으로 해석된 JSON을 돌려준다. 실패하면 null.
    // 연결이 끊기거나 시간이 지나면 그 연결을 맵에서 뺀다
    nlohmann::json sendAndReceive(const std::string& deviceId, const nlohmann::json& request,
                                  std::chrono::milliseconds timeout = kResponseTimeout);

    std::vector<std::string> connectedIds();
    // 맵에서 빼서 돌려준다(없으면 nullptr). 닫기는 호출자가 한다
    std::shared_ptr<TcpConnection> take(const std::string& deviceId);
    // keep에 없는 ID의 연결을 모두 닫는다(커미셔닝 전 좀비 정리)
    void closeConnectionsNotIn(const std::unordered_set<std::string>& keep);

private:
    void acceptLoop();
    // 상한 안이면 맵에 넣는다(같은 ID면 교체해 옛 연결을 outReplaced로). 상한이면 false
    bool tryRegister(const std::string& deviceId, const std::shared_ptr<TcpConnection>& connection,
                     std::shared_ptr<TcpConnection>& outReplaced);
    // 상대가 끊은 연결을 맵에서 뺀다. 뺀 개수를 돌려준다
    std::size_t pruneDeadConnections();
    std::shared_ptr<TcpConnection> find(const std::string& deviceId);
    // 맵의 연결이 아직 같은 객체일 때만 뺀다(그 사이 재접속한 새 연결을 지우지 않는다)
    void removeIfSame(const std::string& deviceId, const std::shared_ptr<TcpConnection>& connection);

    int listenFd_ = -1;
    int boundPort_ = 0;
    std::thread acceptThread_;
    std::atomic<bool> running_{false};

    std::mutex connectionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<TcpConnection>> connections_;
};
