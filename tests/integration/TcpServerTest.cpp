#include <gtest/gtest.h>
#include "TcpProtocol.hpp"
#include "TcpServer.hpp"
#include "json.hpp"
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

// 테스트가 흉내 내는 DIY 기기. 127.0.0.N에 bind해 접속하므로 기기마다 ID(ARD_127.0.0.N)가 다르다
class FakeDevice {
public:
    FakeDevice(int serverPort, int lastOctet) : ip_("127.0.0." + std::to_string(lastOctet)) {
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

    std::string id() const { return "ARD_" + ip_; }
    void send(const std::string& data) { ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL); }
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
    int fd_ = -1;
    std::string pending_;
    std::thread server_;
    std::atomic<bool> stopServing_{false};
};

int openFdCount() {
    int count = 0;
    if (DIR* dir = ::opendir("/proc/self/fd")) {
        while (::readdir(dir)) ++count;
        ::closedir(dir);
    }
    return count;
}

class TcpServerTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(server.start(0, "127.0.0.1")); }
    void TearDown() override { server.stop(); }

    std::unique_ptr<FakeDevice> connect(int lastOctet) {
        auto device = std::make_unique<FakeDevice>(server.boundPort(), lastOctet);
        EXPECT_TRUE(waitUntilConnected(device->id()));
        return device;
    }
    bool waitUntilConnected(const std::string& id, std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& connected : server.connectedIds()) if (connected == id) return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }
    bool isConnected(const std::string& id) {
        for (const auto& connected : server.connectedIds()) if (connected == id) return true;
        return false;
    }

    TcpServer server;
};

// ---- 메시지 호환: 기기 펌웨어가 받는 문자열은 이전과 바이트 단위로 같다 ----

TEST(TcpProtocolTest, RequestLinesAreByteIdenticalToPreviousFormat) {
    json control;
    ASSERT_TRUE(TcpProtocol::buildCommandRequest("ON", control));
    EXPECT_EQ(TcpProtocol::toLine(control), "{\"action\":\"CONTROL\",\"target\":\"RELAY\",\"value\":\"ON\"}\n");
    EXPECT_EQ(TcpProtocol::toLine(TcpProtocol::buildGetStateRequest()), "{\"action\":\"GET_STATE\"}\n");
    EXPECT_EQ(TcpProtocol::resetLine(), "{\"action\":\"RESET\"}\n");

    // PCDriver의 종료 메시지(공백·줄바꿈 포함 JSON)는 다시 직렬화되어 한 줄이 된다
    json shutdown;
    ASSERT_TRUE(TcpProtocol::buildCommandRequest("{\n  \"action\": \"SYSTEM\",\n  \"target\": \"OS\",\n  \"value\": \"SHUTDOWN\"\n}", shutdown));
    EXPECT_EQ(TcpProtocol::toLine(shutdown), "{\"action\":\"SYSTEM\",\"target\":\"OS\",\"value\":\"SHUTDOWN\"}\n");

    json broken;
    EXPECT_FALSE(TcpProtocol::buildCommandRequest("{not json", broken));
}

// ---- 줄 조립 ----

TEST_F(TcpServerTest, ResponseLongerThan512BytesInFragmentsIsAssembled) {
    auto device = connect(2);
    const std::string padding(1500, 'x');
    auto reply = std::async(std::launch::async, [&] {
        EXPECT_FALSE(device->readLine().empty());
        const std::string response = "{\"status\":\"success\",\"pad\":\"" + padding + "\"}\n";
        for (std::size_t offset = 0; offset < response.size(); offset += 400) {
            device->send(response.substr(offset, 400));
            std::this_thread::sleep_for(30ms);
        }
    });

    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    reply.get();

    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result["pad"].get<std::string>().size(), padding.size());
}

TEST_F(TcpServerTest, MultipleLinesUseLastValidJsonAndSkipGarbage) {
    auto device = connect(2);
    device->serveInBackground([](const std::string&) {
        return std::string("not json\r\n{\"state\":\"OFF\"}\n{\"state\":\"ON\"}\n");
    });
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    device->stopServing();
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result["state"], "ON");
}

TEST_F(TcpServerTest, ResponseWithoutTrailingNewlineIsAccepted) {
    auto device = connect(2);
    device->serveInBackground([](const std::string&) { return std::string("{\"state\":\"ON\"}"); });
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    device->stopServing();
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result["state"], "ON");
}

// 해석 안 되는 응답만 오면 실패지만 연결은 유지한다(이전 동작과 같음)
TEST_F(TcpServerTest, GarbageOnlyResponseFailsButKeepsConnection) {
    auto device = connect(2);
    device->serveInBackground([](const std::string&) { return std::string("hello\n"); });
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest(), 300ms);
    device->stopServing();
    EXPECT_TRUE(result.is_null());
    EXPECT_TRUE(isConnected(device->id()));
}

// 요청 전에 와 있던 데이터(늦게 도착한 이전 응답)를 이번 요청의 답으로 쓰지 않는다
TEST_F(TcpServerTest, StaleInputBeforeRequestIsDiscarded) {
    auto device = connect(2);
    device->send("{\"state\":\"STALE\"}\n");
    std::this_thread::sleep_for(50ms);
    device->serveInBackground([](const std::string&) { return std::string("{\"state\":\"FRESH\"}\n"); });
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    device->stopServing();
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result["state"], "FRESH");
}

// ---- 실패 처리 ----

TEST_F(TcpServerTest, NoResponseTimesOutInAboutOneSecondAndDropsConnection) {
    auto device = connect(2);
    const auto started = std::chrono::steady_clock::now();
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_TRUE(result.is_null());
    EXPECT_GE(elapsed, 900ms);
    EXPECT_LT(elapsed, 1500ms);
    EXPECT_FALSE(isConnected(device->id()));
    EXPECT_TRUE(device->waitClosedByServer());
}

TEST_F(TcpServerTest, LineOverLimitClosesConnection) {
    auto device = connect(2);
    device->serveInBackground([](const std::string&) {
        return std::string(TcpConnection::kMaxLineBytes + 100, 'a');  // 줄바꿈 없는 긴 입력
    });
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    device->stopServing();
    EXPECT_TRUE(result.is_null());
    EXPECT_FALSE(isConnected(device->id()));
    EXPECT_TRUE(device->waitClosedByServer());
}

TEST_F(TcpServerTest, PeerClosedConnectionFailsFastAndIsRemoved) {
    auto device = connect(2);
    const std::string id = device->id();
    device->closeSocket();
    std::this_thread::sleep_for(50ms);

    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(server.sendAndReceive(id, TcpProtocol::buildGetStateRequest()).is_null());
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_FALSE(isConnected(id));
}

// ---- 직렬화·재접속·상한 ----

// 같은 기기에 동시에 보낸 요청은 한 쌍씩 처리되어 응답이 섞이지 않는다
TEST_F(TcpServerTest, ConcurrentRequestsToSameDeviceGetTheirOwnResponses) {
    auto device = connect(2);
    device->serveInBackground([](const std::string& request) {
        std::this_thread::sleep_for(20ms);  // 느린 기기
        const json parsed = json::parse(request);
        return json{{"status", "success"}, {"echo", parsed["value"]}}.dump() + "\n";
    });

    constexpr int kThreads = 4;
    constexpr int kPerThread = 5;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string token = "T" + std::to_string(t) + "_" + std::to_string(i);
                const json result = server.sendAndReceive(device->id(), json{{"action", "CONTROL"}, {"value", token}}, 3000ms);
                if (!result.is_object() || result["echo"] != token) ++mismatches;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    device->stopServing();
    EXPECT_EQ(mismatches.load(), 0);
}

TEST_F(TcpServerTest, ReconnectFromSameIpClosesOldConnectionWithoutLeakingFds) {
    auto first = connect(2);
    const int baseline = openFdCount();
    for (int i = 0; i < 10; ++i) {
        auto next = std::make_unique<FakeDevice>(server.boundPort(), 2);
        EXPECT_TRUE(first->waitClosedByServer()) << "재접속 " << i << "에서 옛 연결이 닫히지 않음";
        first = std::move(next);  // 옛 클라이언트 소켓도 닫힌다
    }
    ASSERT_TRUE(waitUntilConnected(first->id()));
    EXPECT_LE(openFdCount(), baseline + 1);
    EXPECT_EQ(server.connectedIds().size(), 1u);
}

TEST_F(TcpServerTest, ConnectionsBeyondLimitAreRejectedImmediately) {
    std::vector<std::unique_ptr<FakeDevice>> devices;
    for (std::size_t i = 0; i < TcpServer::kMaxConnections; ++i) devices.push_back(connect(static_cast<int>(10 + i)));
    ASSERT_EQ(server.connectedIds().size(), TcpServer::kMaxConnections);

    FakeDevice extra(server.boundPort(), 200);
    EXPECT_TRUE(extra.waitClosedByServer());
    EXPECT_EQ(server.connectedIds().size(), TcpServer::kMaxConnections);

    // 이미 있는 기기의 재접속은 상한과 상관없이 교체된다
    auto replacement = std::make_unique<FakeDevice>(server.boundPort(), 10);
    EXPECT_TRUE(devices[0]->waitClosedByServer());
    EXPECT_EQ(server.connectedIds().size(), TcpServer::kMaxConnections);
}

// ---- 해제·정리 ----

TEST_F(TcpServerTest, TakeThenSendResetDeliversExactLineAndCloses) {
    auto device = connect(2);
    auto connection = server.take(device->id());
    ASSERT_NE(connection, nullptr);
    EXPECT_FALSE(isConnected(device->id()));

    EXPECT_TRUE(connection->sendOnly(TcpProtocol::resetLine()));
    connection->shutdown();

    EXPECT_EQ(device->readLine(), "{\"action\":\"RESET\"}\n");
    EXPECT_TRUE(device->waitClosedByServer());
    EXPECT_EQ(server.take("ARD_127.0.0.99"), nullptr);
}

TEST_F(TcpServerTest, CloseConnectionsNotInKeepsOnlyListedIds) {
    auto keep = connect(2);
    auto drop = connect(3);
    server.closeConnectionsNotIn({keep->id()});
    EXPECT_TRUE(isConnected(keep->id()));
    EXPECT_FALSE(isConnected(drop->id()));
    EXPECT_TRUE(drop->waitClosedByServer());
}

// ---- 7c: 읽기 상한·객체만·끊긴 연결 정리·CLOEXEC ----

// 요청 전부터 끝없이 보내는 기기: 요청은 곧 실패하고 연결이 닫히며, 다른 기기 요청은 막히지 않는다
TEST_F(TcpServerTest, EndlessSenderFailsQuicklyWithoutBlockingOthers) {
    auto noisy = connect(2);
    auto quiet = connect(3);
    quiet->serveInBackground([](const std::string&) { return std::string("{\"state\":\"ON\"}\n"); });
    std::atomic<bool> stopFlooding{false};
    std::thread flooder([&] {
        const std::string junk(1024, 'x');
        while (!stopFlooding) noisy->send(junk);
    });
    std::this_thread::sleep_for(50ms);

    auto other = std::async(std::launch::async, [&] {
        return server.sendAndReceive(quiet->id(), TcpProtocol::buildGetStateRequest());
    });
    const auto started = std::chrono::steady_clock::now();
    const json result = server.sendAndReceive(noisy->id(), TcpProtocol::buildGetStateRequest());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto otherStatus = other.wait_for(1s);
    stopFlooding = true;
    flooder.join();
    quiet->stopServing();

    EXPECT_TRUE(result.is_null());
    EXPECT_LT(elapsed, 1500ms);
    EXPECT_FALSE(isConnected(noisy->id()));
    ASSERT_EQ(otherStatus, std::future_status::ready) << "다른 기기 요청이 막혔다";
    EXPECT_EQ(other.get()["state"], "ON");
}

// 유효 줄 뒤에 대량 입력이 이어지면 더 읽기를 상한에서 멈추고 연결을 닫는다(받은 유효 줄로 판단)
TEST_F(TcpServerTest, StreamAfterValidLineStopsAtDrainLimit) {
    auto device = connect(2);
    device->serveInBackground([](const std::string&) {
        return std::string("{\"state\":\"ON\"}\n") + std::string(TcpConnection::kMaxDrainBytes * 2, 'y');
    });
    const auto started = std::chrono::steady_clock::now();
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    device->stopServing();

    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result["state"], "ON");
    EXPECT_LT(elapsed, 1000ms);
    EXPECT_FALSE(isConnected(device->id()));
}

// 너무 많은 줄은 한 요청의 줄 수 상한으로 끊고 연결을 닫는다.
// 상한이 첫 유효 줄 전에 걸리면 실패, 뒤에 걸리면 받은 유효 줄을 쓴다(도착 조각에 따라 다름) — 둘 다 연결은 닫힌다
TEST_F(TcpServerTest, TooManyLinesInOneResponseClosesConnection) {
    auto device = connect(2);
    device->serveInBackground([](const std::string&) {
        std::string many;
        for (std::size_t i = 0; i < TcpConnection::kMaxLinesPerRequest + 10; ++i) many += "{\"state\":\"ON\"}\n";
        return many;
    });
    const auto started = std::chrono::steady_clock::now();
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    device->stopServing();
    EXPECT_TRUE(result.is_null() || result["state"] == "ON");
    EXPECT_LT(elapsed, 1000ms);
    EXPECT_FALSE(isConnected(device->id()));
}

// 숫자·null·배열·문자열 줄은 응답으로 인정하지 않고 객체만 쓴다
TEST_F(TcpServerTest, OnlyJsonObjectsCountAsResponses) {
    auto device = connect(2);
    device->serveInBackground([](const std::string&) {
        return std::string("42\nnull\n[1,2]\n\"text\"\n{\"state\":\"ON\"}\n7\n");
    });
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    device->stopServing();
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result["state"], "ON");
}

// 숫자만 오면 유효 응답이 아니다(예전엔 숫자 줄도 JSON으로 받아 형 오류 예외 위험이 있었다)
TEST_F(TcpServerTest, NumberOnlyResponseIsNotAccepted) {
    auto device = connect(2);
    device->serveInBackground([](const std::string&) { return std::string("1\n"); });
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest(), 300ms);
    device->stopServing();
    EXPECT_TRUE(result.is_null());
}

TEST(TcpProtocolTest, WrongFieldTypesFailWithoutThrowing) {
    std::string error;
    EXPECT_NO_THROW(EXPECT_FALSE(TcpProtocol::isCommandSuccess(json{{"status", 1}, {"message", 2}}, error)));
    EXPECT_EQ(error, "알 수 없는 에러");
    EXPECT_FALSE(TcpProtocol::isCommandSuccess(json{{"status", "error"}, {"message", "busy"}}, error));
    EXPECT_EQ(error, "busy");
    EXPECT_FALSE(TcpProtocol::isCommandSuccess(json(nullptr), error));
    EXPECT_TRUE(TcpProtocol::isCommandSuccess(json{{"status", "success"}}, error));

    EXPECT_NO_THROW(EXPECT_FALSE(TcpProtocol::parseStateResponse(json{{"state", 1}}).has_value()));
    EXPECT_FALSE(TcpProtocol::parseStateResponse(json{{"other", "ON"}}).has_value());
    EXPECT_FALSE(TcpProtocol::parseStateResponse(json(nullptr)).has_value());
    EXPECT_EQ(TcpProtocol::parseStateResponse(json{{"state", "ON"}}).value_or(""), "ON");
}

// 정상 응답 직후 기기가 끊으면, 응답은 쓰고 연결은 맵에서 뺀다
TEST_F(TcpServerTest, DisconnectRightAfterResponseUsesItAndDropsConnection) {
    auto device = connect(2);
    auto reply = std::async(std::launch::async, [&] {
        EXPECT_FALSE(device->readLine().empty());
        device->send("{\"state\":\"ON\"}\n");
        device->closeSocket();
    });
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    reply.get();
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result["state"], "ON");
    // 드레인이 끊김을 못 본 경우에도 다음 요청에서 빠지므로, 둘 중 하나로 맵에서 사라져야 한다
    if (isConnected("ARD_127.0.0.2")) server.sendAndReceive("ARD_127.0.0.2", TcpProtocol::buildGetStateRequest());
    EXPECT_FALSE(isConnected("ARD_127.0.0.2"));
}

// 드레인 뒤 개행 없는 마지막 조각도 유효 객체면 마지막 줄로 쓴다
TEST_F(TcpServerTest, UnterminatedLastFragmentAfterDrainIsLastLine) {
    auto device = connect(2);
    device->serveInBackground([](const std::string&) {
        return std::string("{\"state\":\"OFF\"}\n{\"state\":\"ON\"}");
    });
    const json result = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    device->stopServing();
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result["state"], "ON");
}

// 상한(32)에 닿아도 이미 끊긴 연결이 자리를 차지하고 있으면 정리하고 새 연결을 받는다
TEST_F(TcpServerTest, DeadConnectionsArePrunedWhenLimitIsReached) {
    std::vector<std::unique_ptr<FakeDevice>> devices;
    for (std::size_t i = 0; i < TcpServer::kMaxConnections; ++i) devices.push_back(connect(static_cast<int>(10 + i)));
    const std::string deadId = devices[5]->id();
    devices[5]->closeSocket();
    std::this_thread::sleep_for(50ms);

    FakeDevice newcomer(server.boundPort(), 200);
    EXPECT_TRUE(waitUntilConnected(newcomer.id()));
    EXPECT_FALSE(newcomer.waitClosedByServer(200ms));
    EXPECT_FALSE(isConnected(deadId));
    EXPECT_EQ(server.connectedIds().size(), TcpServer::kMaxConnections);
}

// 서버가 만든 소켓(리슨·수락)은 CLOEXEC라 chip-tool 같은 자식 프로세스에 넘어가지 않는다
TEST(TcpServerCloexecTest, ServerSocketsAreCloseOnExec) {
    auto socketFds = [] {
        std::set<int> fds;
        if (DIR* dir = ::opendir("/proc/self/fd")) {
            while (dirent* entry = ::readdir(dir)) {
                const std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                char target[256] = {0};
                const std::string link = "/proc/self/fd/" + name;
                if (::readlink(link.c_str(), target, sizeof(target) - 1) > 0 && std::string(target).rfind("socket:", 0) == 0) {
                    fds.insert(std::stoi(name));
                }
            }
            ::closedir(dir);
        }
        return fds;
    };
    const std::set<int> before = socketFds();
    TcpServer server;
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    FakeDevice device(server.boundPort(), 2);  // 클라이언트도 CLOEXEC로 만든다
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (server.connectedIds().empty() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    ASSERT_EQ(server.connectedIds().size(), 1u);

    int checked = 0;
    for (int fd : socketFds()) {
        if (before.count(fd)) continue;
        const int flags = ::fcntl(fd, F_GETFD);
        EXPECT_TRUE(flags >= 0 && (flags & FD_CLOEXEC)) << "fd " << fd << "에 CLOEXEC 없음";
        ++checked;
    }
    EXPECT_GE(checked, 3);  // 리슨 + 수락 + 클라이언트
    server.stop();
}

// ---- 정지 ----

TEST(TcpServerStopTest, StopReturnsQuicklyWhileAcceptingAndWithOpenConnections) {
    TcpServer idle;
    ASSERT_TRUE(idle.start(0, "127.0.0.1"));
    auto started = std::chrono::steady_clock::now();
    idle.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);

    TcpServer busy;
    ASSERT_TRUE(busy.start(0, "127.0.0.1"));
    FakeDevice device(busy.boundPort(), 2);
    std::this_thread::sleep_for(50ms);
    started = std::chrono::steady_clock::now();
    busy.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_TRUE(device.waitClosedByServer());
    busy.stop();  // 두 번 불러도 안전
}

TEST(TcpServerStopTest, StartFailsWhenPortIsTaken) {
    TcpServer first;
    ASSERT_TRUE(first.start(0, "127.0.0.1"));
    TcpServer second;
    EXPECT_FALSE(second.start(first.boundPort(), "127.0.0.1"));
}

// ---- 동시성(TSan 빌드에서 의미가 크다) ----

// 여러 기기에 요청·브로드캐스트를 하는 동안 다른 스레드가 재접속·해제·정리를 반복해도 크래시·경쟁이 없다
TEST_F(TcpServerTest, RequestsWhileDevicesReconnectAndAreRemovedAreSafe) {
    std::vector<std::unique_ptr<FakeDevice>> devices;
    for (int i = 0; i < 4; ++i) {
        devices.push_back(connect(2 + i));
        devices.back()->serveInBackground([](const std::string&) { return std::string("{\"status\":\"success\"}\n"); });
    }
    const int churnPort = server.boundPort();
    std::atomic<bool> stopFlag{false};

    std::thread requester([&] {
        while (!stopFlag) {
            for (const auto& id : server.connectedIds()) server.sendAndReceive(id, TcpProtocol::buildGetStateRequest());
        }
    });
    std::thread churner([&] {
        for (int i = 0; i < 30; ++i) {
            FakeDevice transient(churnPort, 50 + (i % 3));
            std::this_thread::sleep_for(5ms);
            if (i % 2 == 0) {
                if (auto connection = server.take(transient.id())) connection->shutdown();
            } else {
                server.closeConnectionsNotIn({devices[0]->id(), devices[1]->id(), devices[2]->id(), devices[3]->id()});
            }
        }
    });
    churner.join();
    stopFlag = true;
    requester.join();
    for (auto& device : devices) device->stopServing();

    for (const auto& device : devices) EXPECT_TRUE(isConnected(device->id()));
}

}  // namespace
