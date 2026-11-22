#include <gtest/gtest.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "HTTPServer.hpp"
#include "PCDriver.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "httplib.h"
#include "json.hpp"
#include "support/RawHttpClient.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using testsupport::findFreePort;

namespace {

constexpr auto kResponsiveLimit = std::chrono::seconds(1);

// 지정 시간 안에 끝나지 않으면 프로세스를 끝낸다. 교착이 생기면 테스트가 영원히 멈추는 대신 실패로 끝나게 한다.
class DeadlockWatchdog {
public:
    explicit DeadlockWatchdog(std::chrono::seconds limit)
        : thread_([this, limit] {
              std::unique_lock<std::mutex> lock(mutex_);
              if (!cv_.wait_for(lock, limit, [this] { return done_; })) {
                  std::cerr << "[LockRulesTest] 제한 시간 초과: 교착 의심" << std::endl;
                  std::abort();
              }
          }) {}
    ~DeadlockWatchdog() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};

class LockRulesTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::MATTER, &matterFake);
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    }

    void addTcpDevice(const std::string& id) {
        manager.addDevice(id, "dev-" + id, ProtocolType::TCP_DIY, {}, &tcpFake);
    }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver matterFake;
    FakeProtocolDriver tcpFake;
};

// 기기 해제(드라이버의 소켓·파일 작업)가 오래 걸려도 다른 요청은 기기 맵 락에서 기다리지 않는다
TEST_F(LockRulesTest, RemoveDeviceDoesNotBlockOtherRequestsWhileUnpairing) {
    DeadlockWatchdog watchdog(std::chrono::seconds(10));
    addTcpDevice("A");
    addTcpDevice("B");

    std::promise<void> unpairEntered;
    std::promise<void> releaseUnpair;
    std::shared_future<void> releaseSignal = releaseUnpair.get_future().share();
    tcpFake.onUnpair = [&unpairEntered, releaseSignal] {
        unpairEntered.set_value();
        releaseSignal.wait_for(std::chrono::seconds(5)); // 실패해도 영원히 멈추지 않게 상한을 둔다
    };

    auto removal = std::async(std::launch::async, [this] { return manager.removeDevice("A"); });
    ASSERT_EQ(unpairEntered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);

    auto otherRequests = std::async(std::launch::async, [this] {
        const auto devices = manager.getAllDevices();
        const auto control = manager.executeCommand("B", "ON");
        const auto state = manager.getDeviceState("B");
        return devices.size() == 1 && control.outcome == CommandOutcome::Succeeded && state == "ON";
    });
    const auto status = otherRequests.wait_for(kResponsiveLimit);
    releaseUnpair.set_value();

    EXPECT_EQ(status, std::future_status::ready) << "해제 중에 다른 요청이 1초 넘게 막혔다";
    EXPECT_TRUE(otherRequests.get());
    EXPECT_TRUE(removal.get());
    EXPECT_TRUE(database.loadActiveDevices().size() == 1u);
}

// 해제가 시작되면 그 기기는 목록에서 먼저 사라진다(해제 도중의 제어는 "없는 기기")
TEST_F(LockRulesTest, DeviceIsHiddenFromListAsSoonAsUnpairStarts) {
    DeadlockWatchdog watchdog(std::chrono::seconds(10));
    addTcpDevice("A");

    std::promise<void> unpairEntered;
    std::promise<void> releaseUnpair;
    std::shared_future<void> releaseSignal = releaseUnpair.get_future().share();
    tcpFake.onUnpair = [&unpairEntered, releaseSignal] {
        unpairEntered.set_value();
        releaseSignal.wait_for(std::chrono::seconds(5));
    };

    auto removal = std::async(std::launch::async, [this] { return manager.removeDevice("A"); });
    ASSERT_EQ(unpairEntered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);

    EXPECT_TRUE(manager.getAllDevices().empty());
    const auto control = manager.executeCommand("A", "ON");
    releaseUnpair.set_value();

    EXPECT_EQ(control.error.code, ErrorCode::DeviceNotFound);
    EXPECT_TRUE(removal.get());
}

// 여러 스레드가 제어·조회·점검·삭제·재등록을 동시에 반복해도 교착·데이터 경쟁이 없어야 한다(TSan 빌드에서 의미가 크다)
TEST_F(LockRulesTest, ConcurrentOperationsFinishWithoutDeadlock) {
    DeadlockWatchdog watchdog(std::chrono::seconds(60));
    const std::vector<std::string> ids = {"D0", "D1", "D2", "D3"};
    for (const auto& id : ids) addTcpDevice(id);
    manager.addDevice("M0", "plug", ProtocolType::MATTER, {}, &matterFake);
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1500, 2000});

    constexpr int kIterations = 150;
    std::atomic<bool> startFlag{false};
    auto waitStart = [&startFlag] { while (!startFlag.load()) std::this_thread::yield(); };

    std::vector<std::thread> workers;
    workers.emplace_back([&] {
        waitStart();
        for (int i = 0; i < kIterations; ++i) manager.executeCommand(ids[i % ids.size()], i % 2 ? "ON" : "OFF");
    });
    workers.emplace_back([&] {
        waitStart();
        for (int i = 0; i < kIterations; ++i) manager.getDeviceState(ids[i % ids.size()]);
    });
    workers.emplace_back([&] {
        waitStart();
        for (int i = 0; i < kIterations; ++i) {
            manager.getAllDevices();
            Device device;
            manager.getDevice("M0", device);
            manager.activeDeviceIds();
        }
    });
    workers.emplace_back([&] {
        waitStart();
        for (int i = 0; i < kIterations / 10; ++i) manager.runHealthCheck();
    });
    workers.emplace_back([&] {
        waitStart();
        for (int i = 0; i < kIterations; ++i) {
            manager.removeDevice("D3");
            addTcpDevice("D3");
        }
    });
    workers.emplace_back([&] {
        waitStart();
        for (int i = 0; i < kIterations; ++i) {
            manager.updateDeviceState(ids[i % ids.size()], i % 3 ? "ON" : "OFFLINE");
            manager.readEnergy("M0");
        }
    });
    startFlag = true;
    for (auto& worker : workers) worker.join();

    // 끝난 뒤에도 맵과 DB가 정상 동작하는지 확인
    EXPECT_EQ(manager.getAllDevices().size(), ids.size() + 1);
    EXPECT_EQ(database.loadActiveDevices().size(), ids.size() + 1);
    EXPECT_EQ(manager.executeCommand("D0", "ON").outcome, CommandOutcome::Succeeded);
}

// 한 기기의 하드웨어 통신(제어·상태 읽기)이 멈춰 있어도 목록 조회와 다른 기기 제어는 1초 안에 끝난다
TEST_F(LockRulesTest, SlowHardwareCallDoesNotBlockOtherDevices) {
    DeadlockWatchdog watchdog(std::chrono::seconds(20));
    addTcpDevice("SLOW");
    addTcpDevice("FAST");

    for (const bool useReadState : {false, true}) {
        std::promise<void> entered;
        std::promise<void> release;
        std::shared_future<void> releaseSignal = release.get_future().share();
        std::atomic<bool> enteredOnce{false};
        auto stallOnSlow = [&entered, &enteredOnce, releaseSignal](const std::string& deviceId) {
            if (deviceId != "SLOW" || enteredOnce.exchange(true)) return;
            entered.set_value();
            releaseSignal.wait_for(std::chrono::seconds(5));
        };
        // 훅은 스레드를 띄우기 전에 설정한다
        tcpFake.onSendCommand = useReadState ? std::function<void(const std::string&)>() : stallOnSlow;
        tcpFake.onReadState = useReadState ? stallOnSlow : std::function<void(const std::string&)>();

        auto slowCall = std::async(std::launch::async, [this, useReadState] {
            if (useReadState) manager.getDeviceState("SLOW");
            else manager.executeCommand("SLOW", "ON");
        });
        ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);

        auto otherRequests = std::async(std::launch::async, [this] {
            const auto devices = manager.getAllDevices();
            const auto control = manager.executeCommand("FAST", "ON");
            return devices.size() == 2 && control.outcome == CommandOutcome::Succeeded;
        });
        const auto status = otherRequests.wait_for(kResponsiveLimit);
        release.set_value();

        EXPECT_EQ(status, std::future_status::ready)
            << (useReadState ? "상태 읽기" : "제어") << " 중에 다른 요청이 1초 넘게 막혔다";
        EXPECT_TRUE(otherRequests.get());
        slowCall.get();
        tcpFake.onSendCommand = nullptr;
        tcpFake.onReadState = nullptr;
    }
}

// 같은 ID를 한 스레드는 지우고 다른 스레드는 다시 등록하기를 동시에 반복한다.
// 맵과 DB 갱신은 한 락으로 묶지 않으므로(락 규칙) 반복 도중에는 둘이 잠깐 어긋날 수 있다.
// 여기서는 교착·경쟁 없이 끝나고, 그 뒤 순차 삭제·등록으로 메모리와 DB가 다시 일치하는지 본다.
TEST_F(LockRulesTest, ConcurrentRemoveAndAddOfSameIdRecoversConsistency) {
    DeadlockWatchdog watchdog(std::chrono::seconds(60));
    constexpr int kIterations = 200;
    std::atomic<bool> startFlag{false};
    auto waitStart = [&startFlag] { while (!startFlag.load()) std::this_thread::yield(); };
    auto inDatabase = [this] {
        for (const auto& device : database.loadActiveDevices()) if (device.id == "X") return true;
        return false;
    };

    std::thread remover([&] {
        waitStart();
        for (int i = 0; i < kIterations; ++i) manager.removeDevice("X");
    });
    std::thread adder([&] {
        waitStart();
        for (int i = 0; i < kIterations; ++i) addTcpDevice("X");
    });
    startFlag = true;
    remover.join();
    adder.join();

    const bool memoryHasDevice = manager.activeDeviceIds().count("X") == 1;
    if (memoryHasDevice != inDatabase()) {
        GTEST_LOG_(INFO) << "반복 직후 메모리·DB 어긋남 관찰(감수하는 경쟁)";
    }

    manager.removeDevice("X");
    EXPECT_EQ(manager.activeDeviceIds().count("X"), 0u);
    EXPECT_FALSE(inDatabase());
    addTcpDevice("X");
    EXPECT_EQ(manager.activeDeviceIds().count("X"), 1u);
    EXPECT_TRUE(inDatabase());
}

// 해제가 예외를 던져도 DB 삭제는 이미 끝나 있다(재시작 뒤 되살아나지 않음)
TEST_F(LockRulesTest, UnpairExceptionStillLeavesDeviceDeletedFromDatabase) {
    addTcpDevice("A");
    tcpFake.throwOnUnpair = true;

    EXPECT_THROW(manager.removeDevice("A"), std::runtime_error);

    EXPECT_TRUE(manager.getAllDevices().empty());
    EXPECT_TRUE(database.loadActiveDevices().empty());
}

// ---- 명령 결과의 state는 응답에 바로 쓰는 값이다 ----

TEST_F(LockRulesTest, CommandResultStateIsUnknownForMissingDevice) {
    const auto result = manager.executeCommand("NOPE", "ON");
    EXPECT_EQ(result.error.code, ErrorCode::DeviceNotFound);
    EXPECT_EQ(result.state, "UNKNOWN");
}

TEST_F(LockRulesTest, CommandResultStateIsNewStateOnSuccessAndOfflineOnFailure) {
    addTcpDevice("A");
    EXPECT_EQ(manager.executeCommand("A", "ON").state, "ON");
    tcpFake.sendCommandResult = false;
    EXPECT_EQ(manager.executeCommand("A", "OFF").state, "OFFLINE");
}

TEST_F(LockRulesTest, CommandResultStateIsCurrentStateWhenBlocked) {
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [this](IMessageSender*) {
        return std::make_shared<PCDriver>(&tcpFake);
    });
    manager.addDevice("PC", "pc", ProtocolType::TCP_DIY, {DriverType::PC_DRIVER}, &tcpFake);
    manager.updateDeviceState("PC", "OFF");

    const auto result = manager.executeCommand("PC", "OFF");

    EXPECT_EQ(result.error.code, ErrorCode::CommandBlocked);
    EXPECT_EQ(result.state, "OFF");
}

TEST_F(LockRulesTest, CommandResultStateIsCurrentStateWhenProtocolDriverMissing) {
    DelayedCommandScheduler bareManagerScheduler{clock};
    DeviceManager bareManager(database, deviceDrivers, bareManagerScheduler, clock);
    bareManager.addDevice("BARE", "bare", ProtocolType::TCP_DIY, {}, nullptr);

    const auto result = bareManager.executeCommand("BARE", "ON");

    EXPECT_EQ(result.error.code, ErrorCode::Internal);
    EXPECT_EQ(result.state, "UNKNOWN");
}

TEST(LockRulesDelayedTest, CommandResultStateIsCurrentStateWhenDelayed) {
    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver tcpFake;
    ASSERT_TRUE(database.init(":memory:"));
    manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [&tcpFake](IMessageSender*) {
        return std::make_shared<PCDriver>(&tcpFake);
    });
    manager.addDevice("PC", "pc", ProtocolType::TCP_DIY, {DriverType::PC_DRIVER}, &tcpFake);
    manager.updateDeviceState("PC", "ON");

    const auto result = manager.executeCommand("PC", "OFF");

    EXPECT_EQ(result.outcome, CommandOutcome::Accepted);
    EXPECT_EQ(result.state, "ON");
}

// ---- 처리하지 못한 예외는 500 + 고정 본문 ----

TEST_F(LockRulesTest, UnhandledExceptionInHandlerReturns500WithFixedBody) {
    // 가짜 설정은 서버 스레드가 뜨기 전에 끝낸다(설정 값은 락으로 보호하지 않는다)
    addTcpDevice("A");
    tcpFake.throwOnSendCommand = true;
    const std::string token = "test-token-0123456789abcdef";
    const int port = findFreePort();
    HTTPServer server(manager, token, port);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitUntilReady(std::chrono::seconds(3)));

    httplib::Client client("127.0.0.1", port);
    const httplib::Headers headers = {{"Authorization", "Bearer " + token}};
    auto response = client.Post("/api/control", headers, R"({"deviceId":"A","command":"ON"})", "application/json");
    auto afterwards = client.Get("/api/devices", headers);
    server.stop();

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"error":{"code":"INTERNAL_ERROR","message":"서버 내부 오류입니다."}})"));
    EXPECT_EQ(response->body.find("fake sendCommand failure"), std::string::npos); // 예외 내용은 응답에 없다
    ASSERT_TRUE(afterwards);
    EXPECT_EQ(afterwards->status, 200); // 서버는 계속 동작한다
}

}  // namespace
