#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "MacResolver.hpp"
#include "TcpConnection.hpp"
#include "../src/json.hpp"

// 식별된 기기 연결이 맵에 들어갔을 때 알리는 값
struct TcpConnectionInfo {
    std::string deviceId;
    std::string ip;
};

// DIY 기기가 접속하는 TCP 서버: 수락 스레드, 기기 ID(TCP_<MAC>) → 연결 맵, 요청·응답.
// 락: connectionsMutex_(맵·미식별 목록, 메모리만)와 연결별 ioMutex_(그 소켓 송수신)를 동시에 쥐지 않는다.
// 맵에서 shared_ptr을 복사한 뒤 맵 락을 풀고 연결을 쓴다.
class TcpServer {
public:
    // PC 에이전트를 뺀 연결(식별 + 미식별) 수의 상한. 에이전트는 이 밖의 예비 칸 1개를 쓴다
    static constexpr std::size_t kMaxConnections = 32;
    static constexpr std::chrono::milliseconds kResponseTimeout{1000};
    // 같은 ID가 다시 접속했을 때 기존 연결이 살아 있는지 묻는 시간
    static constexpr std::chrono::milliseconds kLivenessTimeout{1000};
    // MAC을 못 찾은 연결을 닫기 전까지 두는 시간. 바로 닫으면 펌웨어가 곧바로 재접속해 접속·로그가 폭주한다
    static constexpr std::chrono::milliseconds kDefaultUnidentifiedHold{30000};

    using ConnectionListener = std::function<void(const TcpConnectionInfo&)>;

    // pcAgentId는 "TCP_" + MAC 12자. unidentifiedHold는 시험에서 짧게 줄 때만 바꾼다
    explicit TcpServer(MacResolver& macResolver, std::optional<std::string> pcAgentId = std::nullopt,
                       std::chrono::milliseconds unidentifiedHold = kDefaultUnidentifiedHold);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // 바인드·리슨까지 마치고 수락 스레드를 띄운다. port 0이면 임의 포트(boundPort로 확인)
    // 이미 실행 중이면 false(HTTPServer::start와 같은 규약: true는 이번 호출이 시작했다는 뜻).
    // 포트는 0~65535(0은 임의 포트). start·stop은 여러 스레드가 동시에 불러도 안전하다
    bool start(int port, const std::string& bindAddress = "0.0.0.0");
    // 수락을 멈추고(join) 모든 연결을 닫는다. 여러 번 불러도 안전하다
    void stop();
    // 실행 중이 아니면 0
    int boundPort() const { return boundPort_; }
    const std::optional<std::string>& pcAgentId() const { return pcAgentId_; }

    // 식별된 기기(에이전트 제외) 연결이 맵에 들어갈 때마다 수락 스레드에서 락 없이 부른다.
    // start 전에 한 번만 설정한다(실행 중 바꾸지 않는다)
    void setConnectionListener(ConnectionListener listener) { connectionListener_ = std::move(listener); }
    // MAC을 못 찾아 맵 밖에 두고 있는 연결 수
    std::size_t unidentifiedCount();

    // 요청 JSON 한 줄을 보내고, 받은 줄 중 마지막으로 해석된 JSON을 돌려준다. 실패하면 null.
    // 연결이 끊기거나 시간이 지나면 그 연결을 맵에서 뺀다
    nlohmann::json sendAndReceive(const std::string& deviceId, const nlohmann::json& request,
                                  std::chrono::milliseconds timeout = kResponseTimeout);

    std::vector<std::string> connectedIds();
    // 그 ID 연결의 상대 IP(없으면 nullopt)
    std::optional<std::string> peerIpOf(const std::string& deviceId);
    // 맵에서 빼서 돌려준다(없으면 nullptr). 닫기는 호출자가 한다
    std::shared_ptr<TcpConnection> take(const std::string& deviceId);
    // keep에 없는 ID의 연결을 모두 닫는다(커미셔닝 전 좀비 정리)
    void closeConnectionsNotIn(const std::unordered_set<std::string>& keep);

private:
    enum class RegisterOutcome { Registered, Full, Conflict };
    struct UnidentifiedConnection {
        std::shared_ptr<TcpConnection> connection;
        std::chrono::steady_clock::time_point closeAt;
    };

    void acceptLoop();
    void handleAccepted(int clientFd, const std::string& clientIp);
    void admitUnidentified(const std::shared_ptr<TcpConnection>& connection);
    // 유지 시간이 지난 미식별 연결을 닫는다
    void closeExpiredUnidentified();
    // 다음 미식별 마감까지 남은 시간(최대 1초). 수락 대기 poll의 제한 시간으로 쓴다
    int nextAcceptWaitMs();
    void closeListenSocket();
    // 상한(에이전트 제외 연결 수)에 여유가 있는지. 에이전트는 항상 받는다. connectionsMutex_를 쥐고 부른다
    bool hasRoomLocked(bool isAgent) const;
    // 맵에 넣는다. 같은 ID가 있으면 그 값이 expected일 때만 교체해 옛 연결을 outReplaced로 준다
    // (에이전트는 expected와 상관없이 교체). 다른 연결로 바뀌어 있으면 Conflict, 상한이면 Full
    RegisterOutcome tryRegister(const std::string& deviceId, const std::shared_ptr<TcpConnection>& connection,
                                const std::shared_ptr<TcpConnection>& expected, bool isAgent,
                                std::shared_ptr<TcpConnection>& outReplaced);
    // 기존 연결에 상태 요청을 한 번 보내 JSON 객체 응답이 오면 살아 있다고 본다(맵 락 밖에서 부른다)
    static bool isAlive(const std::shared_ptr<TcpConnection>& connection);
    // 상대가 끊은 연결을 맵·미식별 목록에서 뺀다. 뺀 개수를 돌려준다
    std::size_t pruneDeadConnections();
    std::shared_ptr<TcpConnection> find(const std::string& deviceId);
    // 맵의 연결이 아직 같은 객체일 때만 뺀다(그 사이 재접속한 새 연결을 지우지 않는다)
    void removeIfSame(const std::string& deviceId, const std::shared_ptr<TcpConnection>& connection);

    // start·stop 호출끼리만 직렬화한다. 수락 스레드와 연결 맵 락은 이 락을 잡지 않는다
    std::mutex lifecycleMutex_;
    int listenFd_ = -1;
    std::atomic<int> boundPort_{0};
    std::thread acceptThread_;
    std::atomic<bool> running_{false};

    MacResolver& macResolver_;
    const std::optional<std::string> pcAgentId_;
    const std::chrono::milliseconds unidentifiedHold_;
    ConnectionListener connectionListener_;

    std::mutex connectionsMutex_;  // connections_·unidentified_
    std::unordered_map<std::string, std::shared_ptr<TcpConnection>> connections_;
    std::vector<UnidentifiedConnection> unidentified_;
};
