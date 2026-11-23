#include <gtest/gtest.h>
#include "TcpMessageSender.hpp"
#include "TcpServer.hpp"
#include "fakes/FakeMacResolver.hpp"
#include "support/FakeTcpDevice.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using fake_tcp::FakeDevice;

namespace {

// PCDriver가 보내는 종료 메시지(공백·줄바꿈 포함 JSON). 기기에는 한 줄로 다시 직렬화되어 간다
const std::string kShutdownPayload = R"({
                "action": "SYSTEM",
                "target": "OS",
                "value": "SHUTDOWN"
            })";
const std::string kShutdownLine = "{\"action\":\"SYSTEM\",\"target\":\"OS\",\"value\":\"SHUTDOWN\"}\n";

constexpr int kSwitchA = 2;
constexpr int kSwitchB = 3;
constexpr int kAgent = 9;

// 받은 줄을 기록하고 바로 성공으로 답하는 기기
class RecordingDevice {
public:
    RecordingDevice(int port, int lastOctet) : device_(port, lastOctet) {
        device_.serveInBackground([this](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.push_back(line);
            return std::string("{\"status\":\"success\"}\n");
        });
    }
    ~RecordingDevice() { device_.stopServing(); }
    std::vector<std::string> lines() {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }
    const std::string id() const { return device_.id(); }

private:
    FakeDevice device_;
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

class PcAgentDispatchTest : public ::testing::Test {
protected:
    void startServer(std::optional<std::string> agentId) {
        server = std::make_unique<TcpServer>(resolver, std::move(agentId));
        ASSERT_TRUE(server->start(0, "127.0.0.1"));
        sender = std::make_unique<TcpMessageSender>(*server);
    }
    void TearDown() override {
        if (server) server->stop();
    }
    bool waitUntilConnectedCount(std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (server->connectedIds().size() < count) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }
    bool isConnected(const std::string& id) {
        for (const auto& connected : server->connectedIds()) if (connected == id) return true;
        return false;
    }

    FakeMacResolver resolver;
    std::unique_ptr<TcpServer> server;
    std::unique_ptr<TcpMessageSender> sender;
};

TEST_F(PcAgentDispatchTest, ConfiguredAgentAloneReceivesShutdownAndSwitchesAreUntouched) {
    startServer(FakeMacResolver::loopbackId(kAgent));
    RecordingDevice switchA(server->boundPort(), kSwitchA);
    RecordingDevice switchB(server->boundPort(), kSwitchB);
    RecordingDevice agent(server->boundPort(), kAgent);
    ASSERT_TRUE(waitUntilConnectedCount(3));

    const auto started = std::chrono::steady_clock::now();
    sender->sendToPcAgent(kShutdownPayload);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms) << "응답하는 에이전트에 1초 지연이 없어야 한다";
    std::this_thread::sleep_for(100ms);

    EXPECT_EQ(agent.lines(), std::vector<std::string>{kShutdownLine});
    EXPECT_TRUE(switchA.lines().empty());
    EXPECT_TRUE(switchB.lines().empty());
    EXPECT_TRUE(isConnected(switchA.id()));
    EXPECT_TRUE(isConnected(switchB.id()));
}

TEST_F(PcAgentDispatchTest, MissingAgentConnectionWarnsAndSendsToNobody) {
    startServer(FakeMacResolver::loopbackId(kAgent));
    RecordingDevice switchA(server->boundPort(), kSwitchA);
    RecordingDevice switchB(server->boundPort(), kSwitchB);
    ASSERT_TRUE(waitUntilConnectedCount(2));

    ::testing::internal::CaptureStderr();
    sender->sendToPcAgent(kShutdownPayload);
    const std::string log = ::testing::internal::GetCapturedStderr();
    std::this_thread::sleep_for(100ms);

    EXPECT_NE(log.find("PC 에이전트(" + FakeMacResolver::loopbackId(kAgent) + ") 연결 없음"), std::string::npos) << log;
    EXPECT_NE(log.find("45초 뒤 전원 차단"), std::string::npos);
    EXPECT_TRUE(switchA.lines().empty());
    EXPECT_TRUE(switchB.lines().empty());
}

TEST_F(PcAgentDispatchTest, WithoutAgentSettingEveryConnectionReceivesShutdown) {
    startServer(std::nullopt);
    RecordingDevice switchA(server->boundPort(), kSwitchA);
    RecordingDevice switchB(server->boundPort(), kSwitchB);
    RecordingDevice agent(server->boundPort(), kAgent);
    ASSERT_TRUE(waitUntilConnectedCount(3));

    sender->sendToPcAgent(kShutdownPayload);
    std::this_thread::sleep_for(100ms);

    EXPECT_EQ(agent.lines(), std::vector<std::string>{kShutdownLine});
    EXPECT_EQ(switchA.lines(), std::vector<std::string>{kShutdownLine});
    EXPECT_EQ(switchB.lines(), std::vector<std::string>{kShutdownLine});
}

TEST_F(PcAgentDispatchTest, SendToUsesSameBytesAndReportsUnreachable) {
    startServer(std::nullopt);
    RecordingDevice switchA(server->boundPort(), kSwitchA);
    ASSERT_TRUE(waitUntilConnectedCount(1));

    EXPECT_TRUE(sender->sendTo(switchA.id(), "ON").ok());
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(switchA.lines(), std::vector<std::string>{"{\"action\":\"CONTROL\",\"target\":\"RELAY\",\"value\":\"ON\"}\n"});
    EXPECT_EQ(sender->sendTo(FakeMacResolver::loopbackId(77), "ON").error.code, ErrorCode::DeviceUnreachable);
    EXPECT_EQ(sender->sendTo(switchA.id(), "{broken").error.code, ErrorCode::InvalidRequest);
}

// 에이전트가 응답하지 않거나 오류로 답해도 로그만 남기고(호출자는 45초 뒤 차단), 스위치에는 가지 않는다
TEST_F(PcAgentDispatchTest, SilentOrErrorAgentIsLoggedAndSwitchesStayUntouched) {
    startServer(FakeMacResolver::loopbackId(kAgent));
    RecordingDevice switchA(server->boundPort(), kSwitchA);
    FakeDevice silentAgent(server->boundPort(), kAgent);
    ASSERT_TRUE(waitUntilConnectedCount(2));

    ::testing::internal::CaptureStderr();
    const auto started = std::chrono::steady_clock::now();
    sender->sendToPcAgent(kShutdownPayload);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    std::string log = ::testing::internal::GetCapturedStderr();
    EXPECT_GE(elapsed, 900ms);
    EXPECT_LT(elapsed, 1500ms);
    EXPECT_NE(log.find("종료 메시지 응답 없음 또는 오류 — 45초 뒤 전원 차단"), std::string::npos) << log;
    EXPECT_EQ(silentAgent.readLine(100ms), kShutdownLine);  // 받기는 했다
    EXPECT_TRUE(silentAgent.waitClosedByServer());          // 답이 없으니 서버가 그 연결을 닫았다
    EXPECT_TRUE(switchA.lines().empty());

    // 다시 붙은 에이전트가 오류로 답하는 경우
    FakeDevice errorAgent(server->boundPort(), kAgent);
    errorAgent.serveInBackground([](const std::string&) { return std::string("{\"status\":\"error\",\"message\":\"busy\"}\n"); });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!isConnected(FakeMacResolver::loopbackId(kAgent)) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    ::testing::internal::CaptureStderr();
    sender->sendToPcAgent(kShutdownPayload);
    log = ::testing::internal::GetCapturedStderr();
    errorAgent.stopServing();
    EXPECT_NE(log.find("기기에서 에러 반환: busy"), std::string::npos) << log;
    EXPECT_NE(log.find("종료 메시지 응답 없음 또는 오류"), std::string::npos);
    EXPECT_TRUE(switchA.lines().empty());
}

// 에이전트가 방금 끊겼으면(FIN) 1초 응답을 기다리지 않고 "연결 끊김" + 차단 예고, 보내지 않는다
TEST_F(PcAgentDispatchTest, JustDisconnectedAgentIsReportedImmediatelyWithShutdownWarning) {
    startServer(FakeMacResolver::loopbackId(kAgent));
    RecordingDevice switchA(server->boundPort(), kSwitchA);
    auto agent = std::make_unique<FakeDevice>(server->boundPort(), kAgent);
    ASSERT_TRUE(waitUntilConnectedCount(2));
    agent->closeGracefully();  // 정상 종료(FIN) → 서버 쪽은 POLLRDHUP
    std::this_thread::sleep_for(50ms);  // FIN이 서버에 도착할 시간
    ASSERT_TRUE(isConnected(FakeMacResolver::loopbackId(kAgent))) << "서버는 아직 끊김을 모른다";

    ::testing::internal::CaptureStderr();
    const auto started = std::chrono::steady_clock::now();
    sender->sendToPcAgent(kShutdownPayload);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const std::string log = ::testing::internal::GetCapturedStderr();

    EXPECT_LT(elapsed, 500ms);
    EXPECT_NE(log.find("연결 끊김 — 종료 메시지 못 보냄, 45초 뒤 전원 차단"), std::string::npos) << log;
    EXPECT_FALSE(isConnected(FakeMacResolver::loopbackId(kAgent)));
    EXPECT_TRUE(switchA.lines().empty());
}

// P3: 끊긴 옛 연결이 이미 새 에이전트 연결로 바뀌었으면 새 연결로 보낸다
TEST_F(PcAgentDispatchTest, ReconnectedAgentReceivesShutdownInsteadOfDroppedOldConnection) {
    startServer(FakeMacResolver::loopbackId(kAgent));
    auto oldAgent = std::make_unique<FakeDevice>(server->boundPort(), kAgent);
    ASSERT_TRUE(waitUntilConnectedCount(1));
    oldAgent->closeGracefully();
    RecordingDevice newAgent(server->boundPort(), kAgent);  // 같은 MAC 재접속: 에이전트는 확인 없이 교체(R-2)
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (server->peerIpOf(FakeMacResolver::loopbackId(kAgent)) && std::chrono::steady_clock::now() < deadline) {
        if (!server->dropIfPeerGone(FakeMacResolver::loopbackId(kAgent))) break;  // 교체됐으면 false
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(waitUntilConnectedCount(1));

    ::testing::internal::CaptureStderr();
    sender->sendToPcAgent(kShutdownPayload);
    const std::string log = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(newAgent.lines(), std::vector<std::string>{kShutdownLine}) << log;
    EXPECT_EQ(log.find("연결 끊김"), std::string::npos) << log;
}

// P3: 살아 있는 연결은 dropIfPeerGone이 지우지 않는다
TEST_F(PcAgentDispatchTest, DropIfPeerGoneKeepsLiveConnection) {
    startServer(FakeMacResolver::loopbackId(kAgent));
    RecordingDevice agent(server->boundPort(), kAgent);
    ASSERT_TRUE(waitUntilConnectedCount(1));
    EXPECT_FALSE(server->dropIfPeerGone(agent.id()));
    EXPECT_TRUE(isConnected(agent.id()));
    EXPECT_FALSE(server->dropIfPeerGone("TCP_NOT_CONNECTED"));
}

TEST(TcpAgentConfigTest, ParsesMacIntoAgentIdOrRejectsMalformedValue) {
    EXPECT_FALSE(TcpAgentConfig::parsePcAgentId(nullptr).value->has_value());
    EXPECT_FALSE(TcpAgentConfig::parsePcAgentId("").value->has_value());
    EXPECT_EQ(TcpAgentConfig::parsePcAgentId("a1:b2:c3:d4:e5:f6").value->value_or(""), "TCP_A1B2C3D4E5F6");
    EXPECT_EQ(TcpAgentConfig::parsePcAgentId("A1-B2-C3-D4-E5-F6").value->value_or(""), "TCP_A1B2C3D4E5F6");
    EXPECT_EQ(TcpAgentConfig::parsePcAgentId("a1b2c3d4e5f6").value->value_or(""), "TCP_A1B2C3D4E5F6");
    for (const char* bad : {"zz", "A1B2C3D4E5F", "A1B2C3D4E5F6A", "a1:b2:c3:d4:e5:g6", " a1b2c3d4e5f6", "TCP_A1B2C3D4E5F6",
                            "00:00:00:00:00:00", "000000000000", "ff:ff:ff:ff:ff:ff", "FFFFFFFFFFFF"}) {
        const auto result = TcpAgentConfig::parsePcAgentId(bad);
        EXPECT_FALSE(result.ok()) << bad;
        EXPECT_EQ(result.error.code, ErrorCode::InvalidRequest) << bad;
    }
}

}  // namespace
