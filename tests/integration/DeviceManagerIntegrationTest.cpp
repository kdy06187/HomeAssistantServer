#include <gtest/gtest.h>
#include "DeviceManager.hpp"
#include "DatabaseManager.hpp"
#include "DriverManager.hpp"
#include "PCDriver.hpp"
#include "PeriodicTasks.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include <chrono>

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
        for (const auto& device : database.loadAllDevices()) {
            if (device.id == id) return device;
        }
        return Device();
    }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DeviceManager manager{database, deviceDrivers, clock};
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
    EXPECT_TRUE(database.loadAllDevices().empty());
}

TEST_F(DeviceManagerIntegrationTest, CommissioningWithEmptyDeviceIdRegistersNothingButSucceeds) {
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{});

    EXPECT_TRUE(manager.startCommissioning(ProtocolType::MATTER, "plug", ""));

    EXPECT_TRUE(manager.getAllDevices().empty());
}

TEST_F(DeviceManagerIntegrationTest, ControlSuccessSetsStateOn) {
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");

    EXPECT_TRUE(manager.executeCommand("FAKE_1", "ON"));

    ASSERT_EQ(tcpFake.sentCommands.size(), 1u);
    EXPECT_EQ(tcpFake.sentCommands[0].second, "ON");
    Device device;
    ASSERT_TRUE(manager.getDevice("FAKE_1", device));
    EXPECT_EQ(device.state, "ON");
    EXPECT_EQ(findInDatabase("FAKE_1").state, "ON");
}

// 현재 동작 그대로: 하드웨어 실패여도 반환값은 true이고 상태만 OFFLINE이 된다
TEST_F(DeviceManagerIntegrationTest, ControlFailureReturnsTrueAndMarksOffline) {
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    tcpFake.sendCommandResult = false;

    EXPECT_TRUE(manager.executeCommand("FAKE_1", "ON"));

    Device device;
    ASSERT_TRUE(manager.getDevice("FAKE_1", device));
    EXPECT_EQ(device.state, "OFFLINE");
}

TEST_F(DeviceManagerIntegrationTest, ControlUnknownDeviceReturnsFalse) {
    EXPECT_FALSE(manager.executeCommand("NOPE", "ON"));
}

// 45초 지연 스레드가 테스트 객체보다 오래 살 수 있어, 이 테스트의 객체는 일부러 해제하지 않는다
TEST(DeviceManagerPcDriverTest, OffOnRunningPcSendsShutdownMessageAndDefersHardware) {
    auto* database = new DatabaseManager();
    auto* deviceDrivers = new DriverManager();
    auto* clock = new FakeClock();
    auto* manager = new DeviceManager(*database, *deviceDrivers, *clock);
    auto* tcpFake = new FakeProtocolDriver();
    ASSERT_TRUE(database->init(":memory:"));
    manager->registerDriver(ProtocolType::TCP_DIY, tcpFake);
    deviceDrivers->registerDriver(DriverType::PC_DRIVER, [tcpFake](IMessageSender*) {
        return std::make_shared<PCDriver>(tcpFake);
    });
    manager->startCommissioning(ProtocolType::TCP_DIY, "pc", "", "", "", {DriverType::PC_DRIVER});
    manager->updateDeviceState("FAKE_1", "ON");

    EXPECT_TRUE(manager->executeCommand("FAKE_1", "OFF"));

    ASSERT_EQ(tcpFake->sentMessages.size(), 1u);
    EXPECT_EQ(tcpFake->sentMessages[0].first, "ALL");
    EXPECT_TRUE(tcpFake->sentCommands.empty());
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
    EXPECT_TRUE(database.loadAllDevices().empty());
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
    EXPECT_EQ(database.getEnergyHistory("FAKE_1").size(), 1u);
}

TEST_F(DeviceManagerIntegrationTest, EnergySuccessAccumulatesInDatabase) {
    manager.startCommissioning(ProtocolType::MATTER, "plug", "");
    int power = -1;
    long total = -1;

    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1500, 100});
    ASSERT_TRUE(manager.getDeviceEnergyInfo("FAKE_1", power, total));
    EXPECT_EQ(power, 1500);
    EXPECT_EQ(total, 100);

    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1600, 150});
    ASSERT_TRUE(manager.getDeviceEnergyInfo("FAKE_1", power, total));
    EXPECT_EQ(total, 150);
}

TEST_F(DeviceManagerIntegrationTest, EnergyFailureReturnsFalseAndKeepsStats) {
    manager.startCommissioning(ProtocolType::MATTER, "plug", "");
    int power = 0;
    long total = 0;
    matterFake.energyResult = Result<EnergyReading>::failure(ErrorCode::Internal, "bad");

    EXPECT_FALSE(manager.getDeviceEnergyInfo("FAKE_1", power, total));

    // 통계가 갱신되지 않았다면 첫 기록으로 취급되어 값이 그대로 합계가 된다
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{0, 70});
    ASSERT_TRUE(manager.getDeviceEnergyInfo("FAKE_1", power, total));
    EXPECT_EQ(total, 70);
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
