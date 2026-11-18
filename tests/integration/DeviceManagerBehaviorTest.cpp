#include <gtest/gtest.h>
#include "DeviceManager.hpp"
#include "DatabaseManager.hpp"
#include "DriverManager.hpp"
#include "PCDriver.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"

namespace {

// 기존 동작 보존 경로(헬스체크 분기, 미등록 ID, 드라이버 부착 조건)를 가짜 드라이버로 확인한다.
class DeviceManagerBehaviorTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::MATTER, &matterFake);
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    }
    std::string dbState(const std::string& id) {
        for (const auto& device : database.loadAllDevices()) {
            if (device.id == id) return device.state;
        }
        return "(none)";
    }
    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DeviceManager manager{database, deviceDrivers, clock};
    FakeProtocolDriver matterFake;
    FakeProtocolDriver tcpFake;
};

TEST_F(DeviceManagerBehaviorTest, CommissioningWithoutRegisteredDriverReturnsFalse) {
    DatabaseManager db2;
    ASSERT_TRUE(db2.init(":memory:"));
    DriverManager drivers2;
    DeviceManager bare(db2, drivers2, clock);

    EXPECT_FALSE(bare.startCommissioning(ProtocolType::MATTER, "plug", ""));
}

TEST_F(DeviceManagerBehaviorTest, MatterRecoveryKeepsDriverReportedIdAndName) {
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"1700000000"});

    ASSERT_TRUE(manager.startCommissioning(ProtocolType::MATTER, "plug", "pin"));

    Device device;
    ASSERT_TRUE(manager.getDevice("1700000000", device));
    EXPECT_EQ(device.name, "plug");
    EXPECT_EQ(device.protocol_type, ProtocolType::MATTER);
}

TEST_F(DeviceManagerBehaviorTest, FailedCommissioningDoesNotLeakDriverTypesToLaterSameNamedDevice) {
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [](IMessageSender* s) { return std::make_shared<PCDriver>(s); });
    tcpFake.commissionResult = Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "fail");
    manager.startCommissioning(ProtocolType::TCP_DIY, "desk", "", "", "", {DriverType::PC_DRIVER});
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"FAKE_1"});

    manager.startCommissioning(ProtocolType::TCP_DIY, "desk", "");

    Device device;
    ASSERT_TRUE(manager.getDevice("FAKE_1", device));
    EXPECT_TRUE(device.driver_types.empty());
}

TEST_F(DeviceManagerBehaviorTest, EnergyForUnregisteredIdReturnsTrueWithZeros) {
    int power = -1;
    long total = -1;

    EXPECT_TRUE(manager.getDeviceEnergyInfo("NOPE", power, total));

    EXPECT_EQ(power, 0);
    EXPECT_EQ(total, 0);
}

TEST_F(DeviceManagerBehaviorTest, EnergyForTcpDeviceIsReadFromMatterDriverNotTcp) {
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"ARD_1"});
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{42, 7});
    int power = 0;
    long total = 0;

    ASSERT_TRUE(manager.getDeviceEnergyInfo("ARD_1", power, total));

    EXPECT_EQ(power, 42);
}

TEST_F(DeviceManagerBehaviorTest, EnergyWithoutMatterDriverReturnsTrueWithZeros) {
    DatabaseManager db2;
    ASSERT_TRUE(db2.init(":memory:"));
    DriverManager drivers2;
    DeviceManager only(db2, drivers2, clock);
    only.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    only.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    int power = -1;
    long total = -1;

    EXPECT_TRUE(only.getDeviceEnergyInfo("FAKE_1", power, total));

    EXPECT_EQ(power, 0);
}

TEST_F(DeviceManagerBehaviorTest, HealthCheckRecordsEnergyOnlyForOnlineMatterDevices) {
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"M_1"});
    manager.startCommissioning(ProtocolType::MATTER, "plug", "");
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"T_1"});
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{5, 300});

    manager.runHealthCheck(); // 둘 다 ON
    EXPECT_EQ(database.getEnergyHistory("M_1").size(), 1u);
    EXPECT_TRUE(database.getEnergyHistory("T_1").empty());

    matterFake.stateToReturn = "OFFLINE";
    manager.runHealthCheck();
    EXPECT_EQ(database.getEnergyHistory("M_1").size(), 1u); // OFFLINE이면 기록 없음
    matterFake.stateToReturn = "UNKNOWN";
    manager.runHealthCheck();
    EXPECT_EQ(database.getEnergyHistory("M_1").size(), 1u);
}

TEST_F(DeviceManagerBehaviorTest, HealthCheckUnknownStateZeroesDatabaseState) {
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    manager.executeCommand("FAKE_1", "ON");
    ASSERT_EQ(dbState("FAKE_1"), "ON");
    tcpFake.stateToReturn = "UNKNOWN";

    manager.runHealthCheck();

    EXPECT_EQ(dbState("FAKE_1"), "OFF");
}

TEST_F(DeviceManagerBehaviorTest, HealthCheckExceptionOnOneDeviceDoesNotSkipTheOthers) {
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"M_1"});
    manager.startCommissioning(ProtocolType::MATTER, "plug", "");
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"T_1"});
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    manager.executeCommand("T_1", "ON");
    matterFake.throwOnReadState = true;
    tcpFake.stateToReturn = "OFFLINE";

    EXPECT_NO_THROW(manager.runHealthCheck());

    EXPECT_EQ(dbState("T_1"), "OFF"); // 순회 순서와 무관하게 정상 기기도 점검됨
}

TEST_F(DeviceManagerBehaviorTest, HealthCheckWithNoDevicesDoesNothing) {
    EXPECT_NO_THROW(manager.runHealthCheck());
}

TEST_F(DeviceManagerBehaviorTest, ActiveDeviceIdsReflectsRegistrationAndRemoval) {
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    EXPECT_EQ(manager.activeDeviceIds().count("FAKE_1"), 1u);

    manager.removeDevice("FAKE_1");

    EXPECT_TRUE(manager.activeDeviceIds().empty());
}

TEST_F(DeviceManagerBehaviorTest, RemoveUnknownDeviceReturnsFalse) {
    EXPECT_FALSE(manager.removeDevice("NOPE"));
}

} // namespace
