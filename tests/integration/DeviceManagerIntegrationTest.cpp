#include <gtest/gtest.h>
#include "DeviceManager.hpp"
#include "DatabaseManager.hpp"
#include "support/HistoryTestHelper.hpp"
#include "DriverManager.hpp"
#include "PCDriver.hpp"
#include "PeriodicTasks.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace {

// DB는 메모리, 프로토콜 드라이버는 가짜로 DeviceManager를 조립한다.
class DeviceManagerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::MATTER, &matterFake);
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    }

    Device findInDatabase(const std::string& id) {
        for (const auto& device : database.loadActiveDevices()) {
            if (device.id == id) return device;
        }
        return Device();
    }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver matterFake;
    FakeProtocolDriver tcpFake;
};

TEST_F(DeviceManagerIntegrationTest, CommissioningSuccessRegistersDeviceWithDriverTypes) {
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [](IMessageSender* sender) {
        return std::make_shared<PCDriver>(sender);
    });

    const bool result = manager.startCommissioning(ProtocolType::TCP_DIY, "desk", "10.0.0.1", "ssid", "pw",
                                                   {DriverType::PC_DRIVER});

    EXPECT_TRUE(result);
    ASSERT_EQ(tcpFake.commissionRequests.size(), 1u);
    EXPECT_EQ(tcpFake.commissionRequests[0].name, "desk");
    EXPECT_EQ(tcpFake.commissionRequests[0].ssid, "ssid");
    Device inMemory;
    ASSERT_TRUE(manager.getDevice("FAKE_1", inMemory));
    EXPECT_EQ(inMemory.attached_drivers.size(), 1u);
    const Device saved = findInDatabase("FAKE_1");
    EXPECT_EQ(saved.name, "desk");
    EXPECT_EQ(saved.protocol_type, ProtocolType::TCP_DIY);
    ASSERT_EQ(saved.driver_types.size(), 1u);
    EXPECT_EQ(saved.driver_types[0], DriverType::PC_DRIVER);
}

TEST_F(DeviceManagerIntegrationTest, CommissioningPassesActiveIdsSnapshotToDriver) {
    manager.startCommissioning(ProtocolType::TCP_DIY, "first", "");
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"FAKE_2"});

    manager.startCommissioning(ProtocolType::TCP_DIY, "second", "");

    ASSERT_EQ(tcpFake.commissionActiveIds.size(), 2u);
    EXPECT_TRUE(tcpFake.commissionActiveIds[0].empty());
    EXPECT_EQ(tcpFake.commissionActiveIds[1].count("FAKE_1"), 1u);
}

TEST_F(DeviceManagerIntegrationTest, CommissioningFailureRegistersNothing) {
    tcpFake.commissionResult = Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "fail");

    EXPECT_FALSE(manager.startCommissioning(ProtocolType::TCP_DIY, "desk", ""));

    EXPECT_TRUE(manager.getAllDevices().empty());
    EXPECT_TRUE(database.loadActiveDevices().empty());
}

TEST_F(DeviceManagerIntegrationTest, CommissioningWithEmptyDeviceIdRegistersNothingButSucceeds) {
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{});

    EXPECT_TRUE(manager.startCommissioning(ProtocolType::MATTER, "plug", ""));

    EXPECT_TRUE(manager.getAllDevices().empty());
}

TEST_F(DeviceManagerIntegrationTest, ControlSuccessSetsStateOn) {
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");

    EXPECT_EQ(manager.executeCommand("FAKE_1", "ON").outcome, CommandOutcome::Succeeded);

    ASSERT_EQ(tcpFake.sentCommands.size(), 1u);
    EXPECT_EQ(tcpFake.sentCommands[0].second, "ON");
    Device device;
    ASSERT_TRUE(manager.getDevice("FAKE_1", device));
    EXPECT_EQ(device.state, "ON");
    EXPECT_EQ(findInDatabase("FAKE_1").state, "ON");
}

TEST_F(DeviceManagerIntegrationTest, ControlFailureReturnsUnreachableAndMarksOffline) {
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    tcpFake.sendCommandResult = false;

    const CommandResult result = manager.executeCommand("FAKE_1", "ON");

    EXPECT_EQ(result.outcome, CommandOutcome::Failed);
    EXPECT_EQ(result.error.code, ErrorCode::DeviceUnreachable);

    Device device;
    ASSERT_TRUE(manager.getDevice("FAKE_1", device));
    EXPECT_EQ(device.state, "OFFLINE");
}

TEST_F(DeviceManagerIntegrationTest, ControlUnknownDeviceReturnsDeviceNotFound) {
    const CommandResult result = manager.executeCommand("NOPE", "ON");

    EXPECT_EQ(result.outcome, CommandOutcome::Failed);
    EXPECT_EQ(result.error.code, ErrorCode::DeviceNotFound);
}

TEST(DeviceManagerPcDriverTest, OffOnRunningPcSendsShutdownMessageAndDefersHardware) {
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
    manager.startCommissioning(ProtocolType::TCP_DIY, "pc", "", "", "", {DriverType::PC_DRIVER});
    manager.updateDeviceState("FAKE_1", "ON");

    const CommandResult result = manager.executeCommand("FAKE_1", "OFF");

    EXPECT_EQ(result.outcome, CommandOutcome::Accepted);
    EXPECT_EQ(result.delaySeconds, 45);

    ASSERT_EQ(tcpFake.broadcastMessages.size(), 1u);
    EXPECT_NE(tcpFake.broadcastMessages[0].find("SHUTDOWN"), std::string::npos);
    EXPECT_TRUE(tcpFake.sentMessages.empty());
    EXPECT_TRUE(tcpFake.sentCommands.empty());
}

TEST_F(DeviceManagerIntegrationTest, GetDeviceStateReadsDriverAndUpdatesState) {
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    tcpFake.stateToReturn = "OFF";

    EXPECT_EQ(manager.getDeviceState("FAKE_1"), "OFF");
    EXPECT_EQ(manager.getDeviceState("NOPE"), "UNKNOWN");
}

TEST_F(DeviceManagerIntegrationTest, RemoveDeviceUnpairsAndDeletesFromListAndDatabase) {
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");

    EXPECT_TRUE(manager.removeDevice("FAKE_1"));

    ASSERT_EQ(tcpFake.unpairedIds.size(), 1u);
    EXPECT_EQ(tcpFake.unpairedIds[0], "FAKE_1");
    EXPECT_TRUE(manager.getAllDevices().empty());
    EXPECT_TRUE(database.loadActiveDevices().empty());
}

TEST_F(DeviceManagerIntegrationTest, HealthCheckMarksOfflineDeviceInDatabase) {
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    manager.executeCommand("FAKE_1", "ON");
    ASSERT_EQ(findInDatabase("FAKE_1").state, "ON");
    tcpFake.stateToReturn = "OFFLINE";

    manager.runHealthCheck();

    EXPECT_EQ(findInDatabase("FAKE_1").state, "OFF"); // is_active 0
}

TEST_F(DeviceManagerIntegrationTest, HealthCheckSurvivesDriverExceptionsAndKeepsWorking) {
    manager.startCommissioning(ProtocolType::MATTER, "plug", "");
    matterFake.throwOnReadEnergy = true;

    EXPECT_NO_THROW(manager.runHealthCheck());

    matterFake.throwOnReadEnergy = false;
    matterFake.throwOnReadState = true;
    EXPECT_NO_THROW(manager.runHealthCheck());

    matterFake.throwOnReadState = false;
    matterFake.energyResult = Result<EnergyReading>::failure(ErrorCode::Internal, "bad");
    EXPECT_NO_THROW(manager.runHealthCheck());

    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{5, 300});
    EXPECT_NO_THROW(manager.runHealthCheck());
    EXPECT_EQ(latestLogs(database, "FAKE_1").value->size(), 1u);
}

TEST_F(DeviceManagerIntegrationTest, EnergySuccessAccumulatesInDatabase) {
    manager.startCommissioning(ProtocolType::MATTER, "plug", "");

    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1500, 100});
    auto first = manager.readEnergy("FAKE_1");
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value->activePower_mW, 1500);
    EXPECT_EQ(first.value->totalEnergy_mWh, 100);

    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1600, 150});
    auto second = manager.readEnergy("FAKE_1");
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value->totalEnergy_mWh, 150);
}

TEST_F(DeviceManagerIntegrationTest, EnergyFailureReturnsFalseAndKeepsStats) {
    manager.startCommissioning(ProtocolType::MATTER, "plug", "");
    matterFake.energyResult = Result<EnergyReading>::failure(ErrorCode::Internal, "bad");

    EXPECT_FALSE(manager.readEnergy("FAKE_1").ok());

    // 통계가 갱신되지 않았다면 첫 기록으로 취급되어 값이 그대로 합계가 된다
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{0, 70});
    auto energy = manager.readEnergy("FAKE_1");
    ASSERT_TRUE(energy.ok());
    EXPECT_EQ(energy.value->totalEnergy_mWh, 70);
}

TEST_F(DeviceManagerIntegrationTest, InitFromDatabaseLoadsDevicesAndAttachesDrivers) {
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [](IMessageSender* sender) {
        return std::make_shared<PCDriver>(sender);
    });
    Device stored("PRE_1", "stored", ProtocolType::TCP_DIY);
    stored.driver_types = {DriverType::PC_DRIVER};
    ASSERT_TRUE(database.insertDevice(stored));

    manager.initFromDatabase();

    Device loaded;
    ASSERT_TRUE(manager.getDevice("PRE_1", loaded));
    EXPECT_EQ(loaded.name, "stored");
    ASSERT_EQ(loaded.driver_types.size(), 1u);
    EXPECT_EQ(loaded.attached_drivers.size(), 1u);
}

TEST_F(DeviceManagerIntegrationTest, PeriodicTasksStopReturnsQuickly) {
    PeriodicTasks tasks(manager);
    tasks.start();

    const auto begin = std::chrono::steady_clock::now();
    tasks.stop();

    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(1));
}

} // namespace

// ---- 지연 명령(스케줄러) ----

class DeviceManagerDelayedTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
        deviceDrivers.registerDriver(DriverType::PC_DRIVER, [this](IMessageSender*) {
            return std::make_shared<PCDriver>(&tcpFake);
        });
        ASSERT_TRUE(manager.addDevice("PC", "pc", ProtocolType::TCP_DIY, {DriverType::PC_DRIVER}, &tcpFake));
        manager.updateDeviceState("PC", "ON");
    }
    // 시간을 흘리고 만기 작업을 실행한다(스케줄러 스레드는 켜지 않는다)
    void elapse(std::chrono::seconds amount) {
        clock.advance(amount);
        scheduler.tick(clock.steadyNow());
    }
    std::string storedState(const std::string& id) {
        for (const auto& device : database.loadActiveDevices()) {
            if (device.id == id) return device.state;
        }
        return "";
    }
    std::string stateOf(const std::string& id) {
        Device device;
        manager.getDevice(id, device);
        return device.state;
    }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver tcpFake;
};

TEST_F(DeviceManagerDelayedTest, OnDuringPendingOffCancelsTheOff) {
    ASSERT_EQ(manager.executeCommand("PC", "OFF").outcome, CommandOutcome::Accepted);

    const auto onResult = manager.executeCommand("PC", "ON");
    elapse(std::chrono::seconds(60));

    EXPECT_EQ(onResult.outcome, CommandOutcome::Succeeded);
    EXPECT_FALSE(scheduler.pending("PC").has_value());
    ASSERT_EQ(tcpFake.sentCommands.size(), 1u);
    EXPECT_EQ(tcpFake.sentCommands[0].second, "ON"); // 만기에도 OFF는 가지 않는다
}

TEST_F(DeviceManagerDelayedTest, SecondOffSendsShutdownMessageOnceAndReturnsRemainingSeconds) {
    const auto first = manager.executeCommand("PC", "OFF");
    clock.advance(std::chrono::seconds(10));
    const auto second = manager.executeCommand("PC", "OFF");

    EXPECT_EQ(first.outcome, CommandOutcome::Accepted);
    EXPECT_EQ(first.delaySeconds, 45);
    EXPECT_EQ(second.outcome, CommandOutcome::Accepted);
    EXPECT_EQ(second.delaySeconds, 35);
    EXPECT_EQ(tcpFake.broadcastMessages.size(), 1u);
}

TEST_F(DeviceManagerDelayedTest, RemovingDeviceCancelsPendingCommand) {
    manager.executeCommand("PC", "OFF");

    ASSERT_TRUE(manager.removeDevice("PC"));
    elapse(std::chrono::seconds(60));

    EXPECT_FALSE(scheduler.pending("PC").has_value());
    EXPECT_TRUE(tcpFake.sentCommands.empty());
}

TEST_F(DeviceManagerDelayedTest, DueCommandSuccessSetsStateOff) {
    manager.executeCommand("PC", "OFF");
    elapse(std::chrono::seconds(44));
    EXPECT_EQ(stateOf("PC"), "ON");
    EXPECT_TRUE(tcpFake.sentCommands.empty());

    elapse(std::chrono::seconds(1));

    EXPECT_EQ(stateOf("PC"), "OFF");
    ASSERT_EQ(tcpFake.sentCommands.size(), 1u);
    EXPECT_EQ(tcpFake.sentCommands[0].second, "OFF");
}

TEST_F(DeviceManagerDelayedTest, DueCommandFailureSetsStateOffline) {
    tcpFake.sendCommandResult = false;
    manager.executeCommand("PC", "OFF");

    elapse(std::chrono::seconds(45));

    EXPECT_EQ(stateOf("PC"), "OFFLINE");
}

TEST_F(DeviceManagerDelayedTest, FailureDuringShutdownIsCancelledAndKeepsState) {
    tcpFake.sendCommandResult = false;
    manager.beginShutdown();

    const auto result = manager.executeCommand("PC", "ON");

    EXPECT_EQ(result.outcome, CommandOutcome::Failed);
    EXPECT_EQ(result.error.code, ErrorCode::Cancelled);
    EXPECT_EQ(stateOf("PC"), "ON");
    EXPECT_EQ(storedState("PC"), "ON"); // DB의 is_active도 그대로
}

TEST_F(DeviceManagerDelayedTest, DueCommandFailureDuringShutdownKeepsState) {
    tcpFake.sendCommandResult = false;
    manager.executeCommand("PC", "OFF");
    manager.beginShutdown();

    elapse(std::chrono::seconds(45));

    EXPECT_EQ(stateOf("PC"), "ON");
}

TEST_F(DeviceManagerDelayedTest, RefreshAndHealthCheckDoNotRecordOfflineDuringShutdown) {
    tcpFake.stateToReturn = "OFFLINE";
    manager.beginShutdown();

    const auto refreshed = manager.refreshState("PC", true);
    manager.runHealthCheck();

    EXPECT_FALSE(refreshed.ok());
    EXPECT_EQ(stateOf("PC"), "ON");
    EXPECT_TRUE(tcpFake.energyReadIds.empty());
}

TEST_F(DeviceManagerDelayedTest, RefreshReturningUnknownDuringShutdownIsCancelled) {
    tcpFake.stateToReturn = "UNKNOWN"; // 취소된 Matter 조회가 돌려주는 값
    manager.beginShutdown();

    const auto refreshed = manager.refreshState("PC", true);

    EXPECT_FALSE(refreshed.ok());
    EXPECT_EQ(refreshed.error.code, ErrorCode::Cancelled);
    EXPECT_EQ(stateOf("PC"), "ON");
}

TEST_F(DeviceManagerDelayedTest, UnknownStateOutsideShutdownStaysSuccessful) {
    tcpFake.stateToReturn = "UNKNOWN";

    const auto refreshed = manager.refreshState("PC", true);

    ASSERT_TRUE(refreshed.ok());
    EXPECT_EQ(*refreshed.value, "UNKNOWN");
}

TEST_F(DeviceManagerDelayedTest, OffAfterSchedulerStoppedFailsAsCancelled) {
    manager.beginShutdown();
    scheduler.stopAndCancelAll();

    const auto result = manager.executeCommand("PC", "OFF");

    EXPECT_EQ(result.outcome, CommandOutcome::Failed);
    EXPECT_EQ(result.error.code, ErrorCode::Cancelled);
    EXPECT_TRUE(tcpFake.sentCommands.empty());
    EXPECT_TRUE(tcpFake.broadcastMessages.empty()); // 종료 중에는 PC 종료 메시지도 보내지 않는다
}

// 삭제·재등록·예약·만기가 동시에 반복돼도 데이터 경쟁이 없고, 끝난 뒤 남은 예약이 없다(TSan에서 의미가 크다)
TEST_F(DeviceManagerDelayedTest, ConcurrentRemoveScheduleAndTickAreRaceFree) {
    std::atomic<bool> done{false};
    std::thread ticker([&] {
        while (!done) {
            clock.advance(std::chrono::seconds(10));
            scheduler.tick(clock.steadyNow());
        }
    });
    std::thread controller([&] {
        for (int i = 0; i < 300; ++i) manager.executeCommand("PC", "OFF");
    });
    for (int i = 0; i < 300; ++i) {
        manager.removeDevice("PC");
        manager.addDevice("PC", "pc", ProtocolType::TCP_DIY, {DriverType::PC_DRIVER}, &tcpFake);
    }
    controller.join();
    done = true;
    ticker.join();

    manager.removeDevice("PC");
    elapse(std::chrono::seconds(60));
    EXPECT_FALSE(scheduler.pending("PC").has_value());
}
