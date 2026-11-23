#include <gtest/gtest.h>
#include "TcpProtocol.hpp"
#include "TcpServer.hpp"
#include "json.hpp"
#include "fakes/FakeMacResolver.hpp"
#include "support/FakeTcpDevice.hpp"
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
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

using fake_tcp::FakeDevice;

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
    void SetUp() override {
        server.setConnectionListener([this](const TcpConnectionInfo&) { ++registrations; });
        ASSERT_TRUE(server.start(0, "127.0.0.1"));
    }
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

    bool waitForRegistrations(int count, std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (registrations.load() < count) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    std::atomic<int> registrations{0};
    FakeMacResolver resolver;
    TcpServer server{resolver};
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

// 끊고 다시 접속하는 기기(펌웨어 재접속)는 생존 확인 없이 바로 교체되고 fd가 새지 않는다
TEST_F(TcpServerTest, ReconnectFromSameIpClosesOldConnectionWithoutLeakingFds) {
    auto first = connect(2);
    ASSERT_TRUE(waitForRegistrations(1));
    const int baseline = openFdCount();
    for (int i = 0; i < 10; ++i) {
        first->closeSocket();
        auto next = std::make_unique<FakeDevice>(server.boundPort(), 2);
        EXPECT_TRUE(waitForRegistrations(2 + i, 500ms)) << "재접속 " << i << "이 바로 교체되지 않음";
        first = std::move(next);
    }
    EXPECT_TRUE(isConnected(first->id()));
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

    // 이미 있는 기기의 재접속은 상한과 상관없이 교체된다(기존이 응답하지 않으므로 생존 확인 1초 뒤)
    auto replacement = std::make_unique<FakeDevice>(server.boundPort(), 10);
    EXPECT_TRUE(devices[0]->waitClosedByServer(3000ms));
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
    EXPECT_EQ(server.take(FakeMacResolver::loopbackId(99)), nullptr);
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
    const std::string id = FakeMacResolver::loopbackId(2);
    if (isConnected(id)) server.sendAndReceive(id, TcpProtocol::buildGetStateRequest());
    EXPECT_FALSE(isConnected(id));
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
    FakeMacResolver resolver;
    TcpServer server(resolver);
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
    FakeMacResolver resolver;
    TcpServer idle(resolver);
    ASSERT_TRUE(idle.start(0, "127.0.0.1"));
    auto started = std::chrono::steady_clock::now();
    idle.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);

    TcpServer busy(resolver);
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
    FakeMacResolver resolver;
    TcpServer first(resolver);
    ASSERT_TRUE(first.start(0, "127.0.0.1"));
    TcpServer second(resolver);
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

// ---- 12b: TCP_<MAC> 식별·미식별 유지·같은 ID 생존 확인·PC 에이전트 칸·접속 알림 ----

// MAC을 못 찾은 연결은 맵에 넣지 않고(제어 불가) 유지 시간이 지나면 닫는다
TEST(TcpServerIdentityTest, UnidentifiedConnectionIsNotControllableAndClosedAfterHold) {
    FakeMacResolver resolver;
    resolver.setFailing("127.0.0.7");
    TcpServer server(resolver, std::nullopt, 300ms);
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    FakeDevice device(server.boundPort(), 7);
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + 1s;
    while (server.unidentifiedCount() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    EXPECT_EQ(server.unidentifiedCount(), 1u);
    EXPECT_TRUE(server.connectedIds().empty());

    EXPECT_TRUE(device.waitClosedByServer(2000ms));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, 250ms);
    EXPECT_LT(elapsed, 1500ms);
    EXPECT_EQ(server.unidentifiedCount(), 0u);
}

TEST(TcpServerIdentityTest, DefaultUnidentifiedHoldIsThirtySeconds) {
    EXPECT_EQ(TcpServer::kDefaultUnidentifiedHold, std::chrono::milliseconds(30000));
}

// 기존 연결이 생존 확인에 응답하면 같은 ID의 새 연결을 거부하고 기존을 지킨다
TEST_F(TcpServerTest, SameIdReconnectIsRejectedWhileOldConnectionAnswers) {
    auto original = connect(2);
    original->serveInBackground([](const std::string&) { return std::string("{\"state\":\"ON\"}\n"); });
    FakeDevice impostor(server.boundPort(), 2);
    EXPECT_TRUE(impostor.waitClosedByServer(2000ms));
    EXPECT_EQ(registrations.load(), 1);
    const json result = server.sendAndReceive(original->id(), TcpProtocol::buildGetStateRequest());
    original->stopServing();
    ASSERT_TRUE(result.is_object()) << "기존 연결이 유지되어야 한다";
    EXPECT_EQ(result["state"], "ON");
}

// 기존 연결이 응답하지 않으면 1초 확인 뒤 새 연결로 교체한다
TEST_F(TcpServerTest, SameIdReconnectReplacesSilentOldConnection) {
    auto original = connect(2);
    const auto started = std::chrono::steady_clock::now();
    FakeDevice next(server.boundPort(), 2);
    EXPECT_TRUE(waitForRegistrations(2, 2500ms));
    EXPECT_GE(std::chrono::steady_clock::now() - started, 900ms);
    EXPECT_TRUE(original->waitClosedByServer());
    EXPECT_FALSE(next.waitClosedByServer(200ms));
    EXPECT_EQ(original->readLine(10ms), "");  // 받은 생존 확인 줄은 이미 소비됐거나 닫힘
}

// IP가 바뀐 같은 기기(같은 MAC)의 재접속은 교체되고, 알림에 새 IP가 실린다
TEST(TcpServerIdentityTest, ReconnectWithChangedIpReplacesAndReportsNewIp) {
    FakeMacResolver resolver;
    resolver.setMac("127.0.0.8", FakeMacResolver::loopbackMac(2));
    TcpServer server(resolver);
    std::mutex infoMutex;
    std::vector<TcpConnectionInfo> infos;
    server.setConnectionListener([&](const TcpConnectionInfo& info) {
        std::lock_guard<std::mutex> lock(infoMutex);
        infos.push_back(info);
    });
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    auto old = std::make_unique<FakeDevice>(server.boundPort(), 2);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    auto infoCount = [&] {
        std::lock_guard<std::mutex> lock(infoMutex);
        return infos.size();
    };
    while (infoCount() < 1 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    FakeDevice moved(server.boundPort(), 8);  // 옛 연결은 응답하지 않는다 → 교체
    while (infoCount() < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    EXPECT_TRUE(old->waitClosedByServer());
    std::lock_guard<std::mutex> lock(infoMutex);
    ASSERT_EQ(infos.size(), 2u);
    EXPECT_EQ(infos[0].deviceId, FakeMacResolver::loopbackId(2));
    EXPECT_EQ(infos[0].ip, "127.0.0.2");
    EXPECT_EQ(infos[1].deviceId, FakeMacResolver::loopbackId(2));
    EXPECT_EQ(infos[1].ip, "127.0.0.8");
    EXPECT_EQ(server.connectedIds().size(), 1u);
}

// PC 에이전트는 생존 확인 없이 새 연결이 바로 이기고, 접속 알림을 보내지 않는다
TEST(TcpServerIdentityTest, AgentReconnectReplacesImmediatelyWithoutProbeOrNotification) {
    FakeMacResolver resolver;
    const std::string agentId = FakeMacResolver::loopbackId(9);
    TcpServer server(resolver, agentId);
    std::atomic<int> notified{0};
    server.setConnectionListener([&](const TcpConnectionInfo&) { ++notified; });
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    FakeDevice old(server.boundPort(), 9);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (server.connectedIds().empty() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    ASSERT_EQ(server.connectedIds().size(), 1u);

    const auto started = std::chrono::steady_clock::now();
    FakeDevice next(server.boundPort(), 9);
    EXPECT_EQ(old.readLine(1500ms), "") << "에이전트는 생존 확인 요청을 받지 않아야 한다";
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_FALSE(next.waitClosedByServer(200ms));
    EXPECT_EQ(server.connectedIds().size(), 1u);
    EXPECT_EQ(notified.load(), 0);
}

// 32칸이 차도 에이전트는 예비 칸으로 받고, 그다음 스위치는 거부한다. 미식별 연결도 칸을 쓴다
TEST(TcpServerIdentityTest, AgentUsesReserveSlotAndUnidentifiedCountTowardLimit) {
    FakeMacResolver resolver;
    resolver.setFailing("127.0.0.10");
    const std::string agentId = FakeMacResolver::loopbackId(250);
    TcpServer server(resolver, agentId);
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    std::vector<std::unique_ptr<FakeDevice>> devices;
    devices.push_back(std::make_unique<FakeDevice>(server.boundPort(), 10));  // 미식별 1
    for (std::size_t i = 1; i < TcpServer::kMaxConnections; ++i) {
        devices.push_back(std::make_unique<FakeDevice>(server.boundPort(), static_cast<int>(10 + i)));
    }
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while ((server.connectedIds().size() + server.unidentifiedCount()) < TcpServer::kMaxConnections &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(server.unidentifiedCount(), 1u);
    ASSERT_EQ(server.connectedIds().size(), TcpServer::kMaxConnections - 1);

    FakeDevice agent(server.boundPort(), 250);
    while (server.connectedIds().size() < TcpServer::kMaxConnections && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_FALSE(agent.waitClosedByServer(200ms));
    EXPECT_EQ(server.connectedIds().size(), TcpServer::kMaxConnections);  // 31 + 에이전트

    FakeDevice extra(server.boundPort(), 200);
    EXPECT_TRUE(extra.waitClosedByServer());
    EXPECT_EQ(server.connectedIds().size(), TcpServer::kMaxConnections);
}

// 접속 알림은 식별된 기기에만, 맵 락 없이 부른다(알림 안에서 서버를 불러도 교착이 없다)
TEST(TcpServerIdentityTest, ListenerIsCalledOnlyForIdentifiedDevicesWithoutHoldingMapLock) {
    FakeMacResolver resolver;
    resolver.setFailing("127.0.0.5");
    TcpServer server(resolver, FakeMacResolver::loopbackId(9));
    std::mutex idsMutex;
    std::vector<std::string> notifiedIds;
    server.setConnectionListener([&](const TcpConnectionInfo& info) {
        const auto connected = server.connectedIds();  // 락을 쥔 채 불렀다면 여기서 교착
        std::lock_guard<std::mutex> lock(idsMutex);
        EXPECT_NE(std::find(connected.begin(), connected.end(), info.deviceId), connected.end());
        notifiedIds.push_back(info.deviceId);
    });
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    FakeDevice sw(server.boundPort(), 3);
    FakeDevice agent(server.boundPort(), 9);
    FakeDevice unknown(server.boundPort(), 5);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((server.connectedIds().size() < 2 || server.unidentifiedCount() < 1) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(50ms);
    std::lock_guard<std::mutex> lock(idsMutex);
    ASSERT_EQ(notifiedIds.size(), 1u);
    EXPECT_EQ(notifiedIds[0], FakeMacResolver::loopbackId(3));
}

// 미식별 연결이 있어도 정지는 빠르고 그 연결도 닫힌다
TEST(TcpServerIdentityTest, StopClosesUnidentifiedConnectionsQuickly) {
    FakeMacResolver resolver;
    resolver.setFailing("127.0.0.7");
    TcpServer server(resolver);
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    FakeDevice device(server.boundPort(), 7);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (server.unidentifiedCount() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    ASSERT_EQ(server.unidentifiedCount(), 1u);
    const auto started = std::chrono::steady_clock::now();
    server.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_TRUE(device.waitClosedByServer());
    EXPECT_EQ(server.unidentifiedCount(), 0u);
}

// 같은 ID로 재접속이 겹쳐도(TSan) 맵에는 하나만 남고 연결이 새지 않는다
TEST_F(TcpServerTest, RapidSameIdReconnectsLeaveOneConnection) {
    std::vector<std::unique_ptr<FakeDevice>> clients;
    for (int i = 0; i < 5; ++i) {
        clients.push_back(std::make_unique<FakeDevice>(server.boundPort(), 4));
        clients.back()->serveInBackground([](const std::string&) { return std::string("{\"state\":\"ON\"}\n"); });
    }
    std::this_thread::sleep_for(200ms);
    for (auto& client : clients) client->stopServing();
    EXPECT_EQ(server.connectedIds().size(), 1u);
    EXPECT_EQ(registrations.load(), 1) << "첫 연결이 응답하므로 나머지는 모두 거부";
}

// ---- 12b: 요청 전 잔여 입력 로그 ----

// 응답 뒤 늦게 온 "\r\n"은 다음 요청을 방해하지 않고 "버림" 로그도 남기지 않는다.
// 공백이 아닌 잔여는 로그를 남긴다. 1회 진단은 프로세스에서 한 번만(앞선 시험이 썼을 수 있어 최대 1번으로 확인)
TEST_F(TcpServerTest, TrailingWhitespaceIsDiscardedSilentlyAndDiagnosticAppearsAtMostOnce) {
    auto device = connect(2);
    device->serveInBackground([](const std::string&) { return std::string("{\"state\":\"ON\"}"); });

    ::testing::internal::CaptureStderr();
    ASSERT_TRUE(server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest()).is_object());
    device->send("\r\n");  // 응답보다 늦게 도착한 줄바꿈
    std::this_thread::sleep_for(50ms);
    const json second = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    device->send("{\"x\":1}\n");  // 공백이 아닌 잔여
    std::this_thread::sleep_for(50ms);
    const json third = server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    const std::string log = ::testing::internal::GetCapturedStderr();
    device->stopServing();

    ASSERT_TRUE(second.is_object());
    EXPECT_EQ(second["state"], "ON");
    ASSERT_TRUE(third.is_object());
    const auto firstDiscardLog = log.find("남아 있던 입력");
    ASSERT_NE(firstDiscardLog, std::string::npos) << "공백이 아닌 잔여는 로그";
    EXPECT_EQ(log.find("남아 있던 입력", firstDiscardLog + 1), std::string::npos) << "\\r\\n 잔여는 로그 없음";
    const auto diagnostic = log.find("1회 진단");
    if (diagnostic != std::string::npos) {
        EXPECT_EQ(log.find("1회 진단", diagnostic + 1), std::string::npos);
        EXPECT_NE(log.find("1회 진단): 0d 0a"), std::string::npos) << "이 프로세스의 첫 잔여는 \\r\\n";
    }
}

// 1회 진단은 처음 잔여 내용을 16진으로(최대 16바이트) 남기고, 두 번째 잔여에는 남기지 않는다
TEST_F(TcpServerTest, FirstDiscardDiagnosticShowsAtMostSixteenHexBytesOnce) {
    auto device = connect(3);
    device->serveInBackground([](const std::string&) { return std::string("{\"state\":\"ON\"}\n"); });
    ::testing::internal::CaptureStderr();
    device->send(std::string(40, 'z'));
    std::this_thread::sleep_for(50ms);
    server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    device->send("\r\n");
    std::this_thread::sleep_for(50ms);
    server.sendAndReceive(device->id(), TcpProtocol::buildGetStateRequest());
    const std::string log = ::testing::internal::GetCapturedStderr();
    device->stopServing();
    const auto diagnostic = log.find("1회 진단");
    if (diagnostic != std::string::npos) {  // 앞선 시험이 이미 썼으면 없다
        std::string expected = "1회 진단): 7a";
        for (int i = 1; i < 16; ++i) expected += " 7a";
        expected += " (";
        EXPECT_NE(log.find(expected), std::string::npos) << log;
        EXPECT_EQ(log.find("1회 진단", diagnostic + 1), std::string::npos);
    }
}

}  // namespace
