#include "TcpServer.hpp"
#include "RequestValidation.hpp"
#include "TcpProtocol.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

using json = nlohmann::json;

namespace {

// 기기 응답은 JSON 객체만 인정한다(숫자·null·배열 줄은 응답이 아니다). 예외를 던지지 않는다
json parseObjectOrDiscarded(const std::string& line) {
    json parsed = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return json(json::value_t::discarded);
    return parsed;
}

bool isJsonObjectLine(const std::string& line) {
    return !parseObjectOrDiscarded(line).is_discarded();
}

}  // namespace

TcpServer::TcpServer(MacResolver& macResolver, std::optional<std::string> pcAgentId,
                     std::chrono::milliseconds unidentifiedHold)
    : macResolver_(macResolver), pcAgentId_(std::move(pcAgentId)), unidentifiedHold_(unidentifiedHold) {}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start(int port, const std::string& bindAddress) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (running_) return false;  // 이미 실행 중: 이번 호출이 시작한 것이 아니다
    // 0은 OS가 고르는 임시 포트(시험용)
    if (port < 0 || port > 65535) {
        std::cerr << "[TCPDriver] 에러 : 포트 범위 오류 (" << port << ", 0~65535)" << std::endl;
        return false;
    }

    // CLOEXEC: chip-tool 같은 자식 프로세스가 소켓을 물려받아 계속 열어 두지 않게 한다.
    // NONBLOCK: poll이 준비됐다고 한 뒤 상대가 접속을 취소해도 accept가 막히지 않게 한다(수락된 소켓에는 물려주지 않는다)
    listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listenFd_ < 0) {
        std::cerr << "[TCPDriver] 에러 : 서버 소켓 생성 실패 (" << std::strerror(errno) << ")" << std::endl;
        return false;
    }
    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 포트 충돌 방지 옵션

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (!RequestValidation::isValidIpv4Address(bindAddress) ||
        inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) {
        std::cerr << "[TCPDriver] 에러 : 바인딩 주소 형식 오류 (포트 " << port << ")" << std::endl;
        closeListenSocket();
        return false;
    }
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        const int cause = errno; // close가 errno를 덮어쓰기 전에 보관한다
        std::cerr << "[TCPDriver] 에러 : 주소 " << bindAddress << " 포트 " << port << " 바인딩 실패 (errno="
                  << cause << " " << std::strerror(cause) << ")" << std::endl;
        closeListenSocket();
        return false;
    }
    if (::listen(listenFd_, 10) < 0) {
        const int cause = errno;
        std::cerr << "[TCPDriver] 에러 : 포트 " << port << " 리슨 실패 (errno=" << cause << " "
                  << std::strerror(cause) << ")" << std::endl;
        closeListenSocket();
        return false;
    }
    socklen_t length = sizeof(address);
    getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
    boundPort_ = ntohs(address.sin_port);

    // 스레드를 못 만들면 running_을 켜기 전에 소켓을 정리하고 실패로 돌려준다
    running_ = true;
    try {
        acceptThread_ = std::thread(&TcpServer::acceptLoop, this);
    } catch (const std::system_error& e) {
        running_ = false;
        boundPort_ = 0;
        std::cerr << "[TCPDriver] 에러 : 수락 스레드 생성 실패 (" << e.what() << ")" << std::endl;
        closeListenSocket();
        return false;
    }
    std::cout << "[TCPDriver] " << bindAddress << ":" << boundPort_ << " 에서 클라이언트 연결 대기 중..." << std::endl;
    return true;
}

void TcpServer::closeListenSocket() {
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

void TcpServer::stop() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (running_.exchange(false)) {
        // 막혀 있는 accept를 깨운다(리눅스에서 리슨 소켓 shutdown은 accept를 오류로 돌려보낸다)
        ::shutdown(listenFd_, SHUT_RDWR);
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    closeListenSocket();
    boundPort_ = 0;

    std::unordered_map<std::string, std::shared_ptr<TcpConnection>> closing;
    std::vector<UnidentifiedConnection> closingUnidentified;
    {
        std::lock_guard<std::mutex> mapLock(connectionsMutex_);
        closing.swap(connections_);
        closingUnidentified.swap(unidentified_);
    }
    for (auto& pair : closing) pair.second->shutdown();
    for (auto& item : closingUnidentified) item.connection->shutdown();
}

void TcpServer::acceptLoop() {
    while (running_) {
        closeExpiredUnidentified();
        // 미식별 연결의 마감을 지키려고 accept 대신 poll로 기다린다. stop의 리슨 소켓 shutdown이 poll을 깨운다
        pollfd waitFor{listenFd_, POLLIN, 0};
        const int ready = ::poll(&waitFor, 1, nextAcceptWaitMs());
        if (!running_) break;
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[TCPDriver] 에러 : 연결 대기 실패 (" << std::strerror(errno) << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (ready == 0) continue;

        sockaddr_in clientAddress{};
        socklen_t addressLength = sizeof(clientAddress);
        const int clientFd = ::accept4(listenFd_, reinterpret_cast<sockaddr*>(&clientAddress), &addressLength, SOCK_CLOEXEC);
        if (clientFd < 0) {
            if (!running_) break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) continue;
            std::cerr << "[TCPDriver] 에러 : 클라이언트 연결 수락 실패 (" << std::strerror(errno) << ")" << std::endl;
            // fd 고갈 같은 오류에서 빈 반복으로 CPU를 태우지 않게 잠깐 쉰다
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        char ipText[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &clientAddress.sin_addr, ipText, sizeof(ipText));
        const std::string clientIp(ipText);
        std::cout << "[TCPDriver] 클라이언트 연결 수락 IP : " << clientIp << std::endl;
        handleAccepted(clientFd, clientIp);
    }
}

void TcpServer::handleAccepted(int clientFd, const std::string& clientIp) {
    auto connection = std::make_shared<TcpConnection>(clientFd, clientIp);
    // 기기는 접속 때 ID를 보내지 않는다. IP는 DHCP로 바뀌므로 ARP 표의 MAC으로 ID를 만든다
    const auto mac = macResolver_.resolve(clientIp);
    if (!mac) {
        admitUnidentified(connection);
        return;
    }
    const std::string deviceId = makeTcpDeviceId(*mac);
    const bool isAgent = pcAgentId_ && *pcAgentId_ == deviceId;

    // 같은 MAC이 또 접속했다: 위조·중복일 수 있으니 기존 연결이 살아 있으면 기존을 지킨다.
    // 확인 요청은 맵 락 밖에서 한다(최대 1초). 에이전트는 확인 없이 새 연결이 이긴다(종료 메시지를 못 보내는 쪽이 더 위험)
    const auto existing = find(deviceId);
    if (existing && !isAgent && !existing->isPeerGone() && isAlive(existing)) {
        std::cerr << "[TCPDriver] 같은 ID 연결이 살아 있음, 새 연결 거부: " << deviceId << " " << clientIp << std::endl;
        connection->shutdown();
        return;
    }

    std::shared_ptr<TcpConnection> replaced;
    RegisterOutcome outcome = tryRegister(deviceId, connection, existing, isAgent, replaced);
    if (outcome == RegisterOutcome::Full && pruneDeadConnections() > 0) {
        // 상한에 닿았지만 이미 끊긴 연결이 자리를 차지하고 있었다면 정리 후 한 번 더 시도한다
        outcome = tryRegister(deviceId, connection, existing, isAgent, replaced);
    }
    if (outcome == RegisterOutcome::Full) {
        std::cerr << "[TCPDriver] 동시 연결 상한(" << kMaxConnections << ") 초과, 연결 거부: " << clientIp << std::endl;
        connection->shutdown();
        return;  // connection이 사라지면서 fd가 닫힌다
    }
    if (outcome == RegisterOutcome::Conflict) {
        std::cerr << "[TCPDriver] 확인하는 동안 같은 ID 연결이 바뀜, 새 연결 거부: " << deviceId << " " << clientIp << std::endl;
        connection->shutdown();
        return;
    }
    if (replaced) {
        if (isAgent) {
            std::cerr << "[TCPDriver] 경고 : PC 에이전트 연결 교체: " << replaced->peerIp() << " → " << clientIp << std::endl;
        } else if (replaced->peerIp() != clientIp) {
            std::cerr << "[TCPDriver] 경고 : 같은 기기가 다른 IP로 재접속, 이전 연결을 닫습니다: " << deviceId << " "
                      << replaced->peerIp() << " → " << clientIp << std::endl;
        } else {
            std::cout << "[TCPDriver] 같은 기기 재접속(이전 연결 응답 없음), 이전 연결을 닫습니다: " << deviceId << std::endl;
        }
        replaced->shutdown();
    }
    std::cout << "[TCPDriver] 기기 ID [" << deviceId << "] 와 소켓이 매핑되었습니다." << (isAgent ? " (PC 에이전트)" : "") << std::endl;
    // 에이전트는 등록 기기가 아니므로 알리지 않는다
    if (!isAgent && connectionListener_) connectionListener_(TcpConnectionInfo{deviceId, clientIp});
}

void TcpServer::admitUnidentified(const std::shared_ptr<TcpConnection>& connection) {
    auto tryAdmit = [this, &connection] {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        if (!hasRoomLocked(false)) return false;
        unidentified_.push_back({connection, std::chrono::steady_clock::now() + unidentifiedHold_});
        return true;
    };
    bool admitted = tryAdmit();
    if (!admitted && pruneDeadConnections() > 0) admitted = tryAdmit();
    if (!admitted) {
        std::cerr << "[TCPDriver] 동시 연결 상한(" << kMaxConnections << ") 초과, 연결 거부: " << connection->peerIp() << std::endl;
        connection->shutdown();
        return;
    }
    // 맵에 넣지 않으므로 제어할 수 없다. 잠시 두었다가 닫아 기기가 다시 접속(그때 다시 조회)하게 한다
    std::cerr << "[TCPDriver] 미식별 연결(MAC 조회 실패): " << connection->peerIp() << std::endl;
}

void TcpServer::closeExpiredUnidentified() {
    std::vector<std::shared_ptr<TcpConnection>> expired;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        const auto now = std::chrono::steady_clock::now();
        auto it = std::stable_partition(unidentified_.begin(), unidentified_.end(),
                                        [now](const UnidentifiedConnection& item) { return item.closeAt > now; });
        for (auto expiredIt = it; expiredIt != unidentified_.end(); ++expiredIt) expired.push_back(expiredIt->connection);
        unidentified_.erase(it, unidentified_.end());
    }
    for (auto& connection : expired) {
        std::cout << "[TCPDriver] 미식별 연결 닫음(유지 시간 경과): " << connection->peerIp() << std::endl;
        connection->shutdown();
    }
}

int TcpServer::nextAcceptWaitMs() {
    constexpr std::chrono::milliseconds kMaxWait{1000};
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    if (unidentified_.empty()) return static_cast<int>(kMaxWait.count());
    auto earliest = unidentified_.front().closeAt;
    for (const auto& item : unidentified_) earliest = std::min(earliest, item.closeAt);
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - std::chrono::steady_clock::now());
    // 소수점 아래를 버려 마감보다 일찍 깨면 한 번 더 돌 뿐이라 +1ms
    return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(left.count() + 1, 0, kMaxWait.count()));
}

bool TcpServer::hasRoomLocked(bool isAgent) const {
    if (isAgent) return true;  // 에이전트 칸은 상한 밖(같은 ID는 교체되므로 최대 1개)
    std::size_t used = connections_.size() + unidentified_.size();
    if (pcAgentId_ && connections_.count(*pcAgentId_)) --used;
    return used < kMaxConnections;
}

TcpServer::RegisterOutcome TcpServer::tryRegister(const std::string& deviceId,
                                                  const std::shared_ptr<TcpConnection>& connection,
                                                  const std::shared_ptr<TcpConnection>& expected, bool isAgent,
                                                  std::shared_ptr<TcpConnection>& outReplaced) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto it = connections_.find(deviceId);
    if (it != connections_.end()) {
        // 확인하는 동안 맵의 값이 다른 연결로 바뀌었으면 그 연결을 지우지 않는다(비교 후 교체)
        if (!isAgent && it->second != expected) return RegisterOutcome::Conflict;
        outReplaced = it->second;
        it->second = connection;
        return RegisterOutcome::Registered;
    }
    // 기존 연결이 그 사이 해제·정리로 빠졌으면 새 칸으로 넣는다
    if (!hasRoomLocked(isAgent)) return RegisterOutcome::Full;
    connections_[deviceId] = connection;
    return RegisterOutcome::Registered;
}

bool TcpServer::isAlive(const std::shared_ptr<TcpConnection>& connection) {
    const auto lines = connection->request(TcpProtocol::toLine(TcpProtocol::buildGetStateRequest()),
                                           kLivenessTimeout, isJsonObjectLine);
    return lines && std::any_of(lines->begin(), lines->end(), isJsonObjectLine);
}

std::size_t TcpServer::unidentifiedCount() {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return unidentified_.size();
}

std::size_t TcpServer::pruneDeadConnections() {
    std::vector<std::pair<std::string, std::shared_ptr<TcpConnection>>> snapshot;
    std::vector<std::shared_ptr<TcpConnection>> unidentifiedSnapshot;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        snapshot.assign(connections_.begin(), connections_.end());
        for (const auto& item : unidentified_) unidentifiedSnapshot.push_back(item.connection);
    }
    std::size_t pruned = 0;
    for (auto& pair : snapshot) {  // 끊김 확인(poll)은 맵 락 밖에서
        if (!pair.second->isPeerGone()) continue;
        removeIfSame(pair.first, pair.second);
        pair.second->shutdown();
        ++pruned;
        std::cout << "[TCPDriver] 끊긴 연결 정리: " << pair.first << std::endl;
    }
    for (auto& connection : unidentifiedSnapshot) {
        if (!connection->isPeerGone()) continue;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            unidentified_.erase(std::remove_if(unidentified_.begin(), unidentified_.end(),
                                               [&connection](const UnidentifiedConnection& item) {
                                                   return item.connection == connection;
                                               }),
                                unidentified_.end());
        }
        connection->shutdown();
        ++pruned;
        std::cout << "[TCPDriver] 끊긴 미식별 연결 정리: " << connection->peerIp() << std::endl;
    }
    return pruned;
}

std::shared_ptr<TcpConnection> TcpServer::find(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto it = connections_.find(deviceId);
    return it == connections_.end() ? nullptr : it->second;
}

void TcpServer::removeIfSame(const std::string& deviceId, const std::shared_ptr<TcpConnection>& connection) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto it = connections_.find(deviceId);
    if (it != connections_.end() && it->second == connection) connections_.erase(it);
}

std::vector<std::string> TcpServer::connectedIds() {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    std::vector<std::string> ids;
    ids.reserve(connections_.size());
    for (const auto& pair : connections_) ids.push_back(pair.first);
    return ids;
}

std::optional<std::string> TcpServer::peerIpOf(const std::string& deviceId) {
    auto connection = find(deviceId);
    if (!connection) return std::nullopt;
    return connection->peerIp();
}

std::shared_ptr<TcpConnection> TcpServer::take(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto it = connections_.find(deviceId);
    if (it == connections_.end()) return nullptr;
    auto connection = std::move(it->second);
    connections_.erase(it);
    return connection;
}

void TcpServer::closeConnectionsNotIn(const std::unordered_set<std::string>& keep) {
    std::vector<std::pair<std::string, std::shared_ptr<TcpConnection>>> closing;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            if (keep.count(it->first) == 0) {
                closing.emplace_back(it->first, std::move(it->second));
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& pair : closing) {
        std::cout << "🧹 [TCPDriver] 미등록 좀비 기기 소켓 강제 종료 및 삭제: " << pair.first << std::endl;
        pair.second->shutdown();
    }
}

json TcpServer::sendAndReceive(const std::string& deviceId, const json& request, std::chrono::milliseconds timeout) {
    auto connection = find(deviceId);
    if (!connection) {
        std::cerr << "[TCPDriver] 기기 소켓을 찾을 수 없습니다: " << deviceId << std::endl;
        return nullptr;
    }

    const auto lines = connection->request(request.dump() + "\n", timeout, isJsonObjectLine);
    if (connection->isClosed()) removeIfSame(deviceId, connection);
    if (!lines) {
        std::cerr << "[TCPDriver] 응답 없음 : " << deviceId << std::endl;
        return nullptr;
    }

    // 응답이 여러 줄이면 마지막으로 해석된 JSON을 쓴다(기존 동작)
    json lastValidJson = nullptr;
    for (const auto& line : *lines) {
        json parsed = parseObjectOrDiscarded(line);
        if (parsed.is_discarded()) {
            std::cerr << "[TCPDriver] JSON 파싱 에러 (응답: " << line << ")" << std::endl;
            continue;
        }
        lastValidJson = std::move(parsed);
    }
    return lastValidJson;
}
