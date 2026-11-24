#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "DeviceManager.hpp"
#include "DatabaseManager.hpp"
#include "support/HistoryTestHelper.hpp"
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
        for (const auto& device : database.loadActiveDevices()) {
            if (device.id == id) return device.state;
        }
        return "(none)";
    }
    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver matterFake;
    FakeProtocolDriver tcpFake;
};

TEST_F(DeviceManagerBehaviorTest, CommissioningWithoutRegisteredDriverReturnsFalse) {
    DatabaseManager db2;
    ASSERT_TRUE(db2.init(":memory:"));
    DriverManager drivers2;
    DelayedCommandScheduler bareScheduler{clock};
    DeviceManager bare(db2, drivers2, bareScheduler, clock);

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
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [this]() { return std::make_shared<PCDriver>(&tcpFake); });
    tcpFake.commissionResult = Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "fail");
    manager.startCommissioning(ProtocolType::TCP_DIY, "desk", "", "", "", {DriverType::PC_DRIVER});
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"FAKE_1"});

    manager.startCommissioning(ProtocolType::TCP_DIY, "desk", "");

    Device device;
    ASSERT_TRUE(manager.getDevice("FAKE_1", device));
    EXPECT_TRUE(device.driver_types.empty());
}

// 커미셔닝 도중 종료가 시작되면 드라이버가 취소 플래그를 보고 끝내고, 아무것도 등록되지 않는다
TEST_F(DeviceManagerBehaviorTest, ShutdownDuringCommissioningCancelsWithoutRegistering) {
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"FAKE_1"});
    tcpFake.onCommission = [this] { manager.beginShutdown(); };

    EXPECT_FALSE(manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", ""));

    EXPECT_TRUE(manager.getAllDevices().empty());
    EXPECT_TRUE(database.loadActiveDevices().empty());
}

// P2: 드라이버가 성공을 돌려준 직후 종료가 시작되면 등록하지 않고 드라이버 쪽 등록을 정리한다
TEST_F(DeviceManagerBehaviorTest, ShutdownRightAfterDriverSuccessDoesNotRegister) {
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"FAKE_1"});
    tcpFake.ignoreCancel = true;
    tcpFake.onCommission = [this] { manager.beginShutdown(); };

    EXPECT_FALSE(manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", ""));

    EXPECT_TRUE(manager.getAllDevices().empty());
    EXPECT_TRUE(database.loadActiveDevices().empty());
    // 드라이버에는 "등록 안 함"을 알려 그쪽이 만든 것만 정리하게 한다(TCP는 RESET)
    EXPECT_EQ(tcpFake.finalizedOutcomes, (std::vector<std::pair<std::string, bool>>{{"FAKE_1", false}}));
}

// P8: 종료가 시작된 뒤에는 드라이버를 부르지 않는다
TEST_F(DeviceManagerBehaviorTest, CommissioningAfterShutdownDoesNotCallDriver) {
    manager.beginShutdown();
    EXPECT_FALSE(manager.startCommissioning(ProtocolType::MATTER, "plug", "pin"));
    EXPECT_TRUE(matterFake.commissionRequests.empty());
}

// P6: 커미셔닝과 종료 시작이 동시에 일어나도 경합 없이 끝난다(TSan)
TEST_F(DeviceManagerBehaviorTest, ConcurrentShutdownAndCommissioningIsRaceFree) {
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"FAKE_1"});
    std::thread commissioner([this] {
        for (int i = 0; i < 50; ++i) manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    });
    std::thread stopper([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        manager.beginShutdown();
    });
    commissioner.join();
    stopper.join();
    EXPECT_FALSE(manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", ""));
}

TEST_F(DeviceManagerBehaviorTest, EnergyForUnregisteredIdFailsWithoutReadingDriver) {
    const auto energy = manager.readEnergy("NOPE");

    ASSERT_FALSE(energy.ok());
    EXPECT_EQ(energy.error.code, ErrorCode::DeviceNotFound);
    EXPECT_TRUE(matterFake.energyReadIds.empty());
}

// TCP 기기는 프로토콜을 보고 곧바로 거부한다. 어느 드라이버에도 묻지 않는다
TEST_F(DeviceManagerBehaviorTest, EnergyForTcpDeviceIsUnsupportedWithoutAskingAnyDriver) {
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"ARD_1"});
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    tcpFake.energyResult = Result<EnergyReading>::failure(ErrorCode::UnsupportedOperation, "unsupported");
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{42, 7});

    const auto energy = manager.readEnergy("ARD_1");

    ASSERT_FALSE(energy.ok());
    EXPECT_EQ(energy.error.code, ErrorCode::UnsupportedOperation);
    EXPECT_TRUE(matterFake.energyReadIds.empty());
    EXPECT_TRUE(tcpFake.energyReadIds.empty());
}

TEST_F(DeviceManagerBehaviorTest, EnergyWithoutProtocolDriverFailsAsInternal) {
    DatabaseManager db2;
    ASSERT_TRUE(db2.init(":memory:"));
    DriverManager drivers2;
    DelayedCommandScheduler bareScheduler{clock};
    DeviceManager bare(db2, drivers2, bareScheduler, clock);
    bare.addDevice("PLUG", "plug", ProtocolType::MATTER, {});

    const auto energy = bare.readEnergy("PLUG");

    ASSERT_FALSE(energy.ok());
    EXPECT_EQ(energy.error.code, ErrorCode::Internal);
}

TEST_F(DeviceManagerBehaviorTest, HealthCheckRecordsEnergyOnlyForOnlineMatterDevices) {
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"M_1"});
    manager.startCommissioning(ProtocolType::MATTER, "plug", "");
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"T_1"});
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{5, 300});

    manager.runHealthCheck(); // 둘 다 ON
    EXPECT_EQ(latestLogs(database, "M_1").value->size(), 1u);
    EXPECT_TRUE(latestLogs(database, "T_1").value->empty());

    matterFake.stateToReturn = "OFFLINE";
    manager.runHealthCheck();
    EXPECT_EQ(latestLogs(database, "M_1").value->size(), 1u); // OFFLINE이면 기록 없음
    matterFake.stateToReturn = "UNKNOWN";
    manager.runHealthCheck();
    EXPECT_EQ(latestLogs(database, "M_1").value->size(), 1u);
}

// 예전에는 UNKNOWN이면 DB를 OFF(0)로 덮었다. 이제 "모름"은 저장된 상태를 바꾸지 않는다
TEST_F(DeviceManagerBehaviorTest, HealthCheckUnknownStateKeepsDatabaseState) {
    manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", "");
    manager.executeCommand("FAKE_1", "ON");
    ASSERT_EQ(dbState("FAKE_1"), "ON");
    tcpFake.stateToReturn = "UNKNOWN";

    manager.runHealthCheck();

    EXPECT_EQ(dbState("FAKE_1"), "ON");
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
    EXPECT_FALSE(manager.removeDevice("NOPE").ok());
}

// 오프라인 기기는 해제가 실패하지만 DB·메모리에서 숨겼으면 삭제 성공이다(unpaired=false)
TEST_F(DeviceManagerBehaviorTest, RemoveSucceedsWhenUnpairFails) {
    manager.addDevice("ARD_1", "lamp", ProtocolType::TCP_DIY, {});
    tcpFake.unpairResult = false;

    const auto outcome = manager.removeDevice("ARD_1");

    ASSERT_TRUE(outcome.ok());
    EXPECT_FALSE(outcome.value->unpaired);

    Device device;
    EXPECT_FALSE(manager.getDevice("ARD_1", device));
    EXPECT_EQ(dbState("ARD_1"), "(none)");
    ASSERT_EQ(tcpFake.unpairedIds.size(), 1u);
}

// OFFLINE은 getDeviceState 경로에서 한 번 저장된다(재시작 뒤 OFF로 보임)
TEST_F(DeviceManagerBehaviorTest, HealthCheckOfflineStoresInactiveOnce) {
    manager.addDevice("ARD_1", "lamp", ProtocolType::TCP_DIY, {});
    ASSERT_TRUE(manager.updateDeviceState("ARD_1", "ON"));
    tcpFake.stateToReturn = "OFFLINE";

    manager.runHealthCheck();

    EXPECT_EQ(dbState("ARD_1"), "OFF");
    Device device;
    ASSERT_TRUE(manager.getDevice("ARD_1", device));
    EXPECT_EQ(device.state, "OFFLINE");
}

// 이력 조회의 DB 오류 코드(limit<=0 → InvalidRequest)를 Internal로 바꾸지 않는다
TEST_F(DeviceManagerBehaviorTest, EnergyHistoryKeepsDbErrorCode) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});

    HistoryQuery query;
    query.limit = 0;
    const auto history = manager.getEnergyHistory("PLUG", query);

    ASSERT_FALSE(history.ok());
    EXPECT_EQ(history.error.code, ErrorCode::InvalidRequest);
}

} // namespace
