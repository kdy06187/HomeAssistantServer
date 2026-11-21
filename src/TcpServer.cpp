#include "TcpServer.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
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

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start(int port, const std::string& bindAddress) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (running_) return true;

    // CLOEXEC: chip-tool 같은 자식 프로세스가 소켓을 물려받아 계속 열어 두지 않게 한다
    listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        std::cerr << "[TCPDriver] 에러 : 서버 소켓 생성 실패 (" << std::strerror(errno) << ")" << std::endl;
        return false;
    }
    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 포트 충돌 방지 옵션

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) {
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
        std::cerr << "[TCPDriver] 에러 : 수락 스레드 생성 실패 (" << e.what() << ")" << std::endl;
        closeListenSocket();
        return false;
    }
    std::cout << "[TCPDriver] " << boundPort_ << " 포트에서 클라이언트 연결 대기 중..." << std::endl;
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

    std::unordered_map<std::string, std::shared_ptr<TcpConnection>> closing;
    {
        std::lock_guard<std::mutex> mapLock(connectionsMutex_);
        closing.swap(connections_);
    }
    for (auto& pair : closing) pair.second->shutdown();
}

void TcpServer::acceptLoop() {
    while (running_) {
        sockaddr_in clientAddress{};
        socklen_t addressLength = sizeof(clientAddress);
        const int clientFd = ::accept4(listenFd_, reinterpret_cast<sockaddr*>(&clientAddress), &addressLength, SOCK_CLOEXEC);
        if (clientFd < 0) {
            if (!running_) break;
            if (errno == EINTR) continue;
            std::cerr << "[TCPDriver] 에러 : 클라이언트 연결 수락 실패 (" << std::strerror(errno) << ")" << std::endl;
            // fd 고갈 같은 오류에서 빈 반복으로 CPU를 태우지 않게 잠깐 쉰다
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        char ipText[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &clientAddress.sin_addr, ipText, sizeof(ipText));
        const std::string clientIp(ipText);
        std::cout << "[TCPDriver] 클라이언트 연결 수락 IP : " << clientIp << std::endl;

        auto connection = std::make_shared<TcpConnection>(clientFd, clientIp);
        // 기기는 접속 때 ID를 보내지 않으므로 접속 IP로 ID를 만든다
        const std::string deviceId = "ARD_" + clientIp;

        std::shared_ptr<TcpConnection> replaced;
        bool rejected = !tryRegister(deviceId, connection, replaced);
        if (rejected && pruneDeadConnections() > 0) {
            // 상한에 닿았지만 이미 끊긴 연결이 자리를 차지하고 있었다면 정리 후 한 번 더 시도한다
            rejected = !tryRegister(deviceId, connection, replaced);
        }
        if (rejected) {
            std::cerr << "[TCPDriver] 동시 연결 상한(" << kMaxConnections << ") 초과, 연결 거부: " << clientIp << std::endl;
            connection->shutdown();
            continue;  // connection이 사라지면서 fd가 닫힌다
        }
        if (replaced) {
            std::cout << "[TCPDriver] 같은 기기 재접속, 이전 연결을 닫습니다: " << deviceId << std::endl;
            replaced->shutdown();
        }
        std::cout << "[TCPDriver] 기기 ID [" << deviceId << "] 와 소켓이 매핑되었습니다." << std::endl;
    }
}

bool TcpServer::tryRegister(const std::string& deviceId, const std::shared_ptr<TcpConnection>& connection,
                            std::shared_ptr<TcpConnection>& outReplaced) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto it = connections_.find(deviceId);
    if (it == connections_.end() && connections_.size() >= kMaxConnections) return false;
    if (it != connections_.end()) outReplaced = it->second;
    connections_[deviceId] = connection;
    return true;
}

std::size_t TcpServer::pruneDeadConnections() {
    std::vector<std::pair<std::string, std::shared_ptr<TcpConnection>>> snapshot;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        snapshot.assign(connections_.begin(), connections_.end());
    }
    std::size_t pruned = 0;
    for (auto& pair : snapshot) {  // 끊김 확인(poll)은 맵 락 밖에서
        if (!pair.second->isPeerGone()) continue;
        removeIfSame(pair.first, pair.second);
        pair.second->shutdown();
        ++pruned;
        std::cout << "[TCPDriver] 끊긴 연결 정리: " << pair.first << std::endl;
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
