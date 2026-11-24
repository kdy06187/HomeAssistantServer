#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "UtcTime.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "support/HistoryTestHelper.hpp"

using std::chrono::hours;
using std::chrono::seconds;

namespace {

const std::chrono::system_clock::time_point kNow = *UtcTime::fromDbText("2026-11-24 00:00:00");
const auto kRestorePeriod = hours(24 * DeviceManager::kRetentionDays);

std::string textAt(std::chrono::system_clock::time_point time) { return UtcTime::toDbText(time); }

// 한 번 열리면 계속 열려 있는 문(스레드 사이 "여기까지 왔다"·"이제 가도 된다" 신호)
class Gate {
public:
    void open() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            opened_ = true;
        }
        cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return opened_; });
    }
    bool waitFor(std::chrono::milliseconds limit) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, limit, [this] { return opened_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool opened_ = false;
};

class RegisterCommissionedTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::MATTER, &matterFake);
    }
    Result<RegisterOutcome> registerPlug(const std::string& id, const std::string& name = "plug") {
        return manager.registerCommissioned(ProtocolType::MATTER, CommissionOutcome{id}, name, {});
    }
    void hide(const std::string& id, std::chrono::system_clock::time_point at) {
        // 메모리에 올리지 않고 DB에만 "그 시각에 숨긴 기기"를 만든다
        ASSERT_TRUE(database.insertDevice(Device(id, "old", ProtocolType::MATTER)));
        ASSERT_TRUE(database.insertEnergyLog(id, 500, textAt(at - hours(1))));
        ASSERT_TRUE(database.markDeleted(id, textAt(at)).ok());
    }
    std::vector<std::pair<std::string, bool>> finalized() {
        std::lock_guard<std::mutex> lock(matterFake.recordMutex);
        return matterFake.finalizedOutcomes;
    }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock{{}, kNow};
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver matterFake;
};

}  // namespace

TEST_F(RegisterCommissionedTest, NewDeviceIsSavedThenConfirmedToDriver) {
    const auto registered = registerPlug("SN-NEW");

    ASSERT_TRUE(registered.ok());
    EXPECT_EQ(registered.value->deviceId, "SN-NEW");
    EXPECT_FALSE(registered.value->restored);
    EXPECT_EQ(database.findDevice("SN-NEW").value->kind, DeviceRecordState::Kind::Active);
    EXPECT_EQ(manager.activeDeviceIds().count("SN-NEW"), 1u);
    EXPECT_EQ(finalized(), (std::vector<std::pair<std::string, bool>>{{"SN-NEW", true}}));
}

// 활성 기기는 덮어쓰지 않고 드라이버에는 "등록 안 함"을 알린다(새로 만든 것만 정리)
TEST_F(RegisterCommissionedTest, ActiveDeviceIsAlreadyRegisteredAndNothingChanges) {
    ASSERT_TRUE(manager.addDevice("SN-1", "living room", ProtocolType::MATTER, {}));

    const auto registered = registerPlug("SN-1", "new name");

    ASSERT_FALSE(registered.ok());
    EXPECT_EQ(registered.error.code, ErrorCode::AlreadyRegistered);
    Device device;
    ASSERT_TRUE(manager.getDevice("SN-1", device));
    EXPECT_EQ(device.name, "living room");
    EXPECT_EQ(database.loadActiveDevices()[0].name, "living room");
    EXPECT_EQ(finalized(), (std::vector<std::pair<std::string, bool>>{{"SN-1", false}}));
}

TEST_F(RegisterCommissionedTest, HiddenDeviceWithinPeriodIsRestoredWithHistory) {
    hide("SN-1", kNow - hours(24));

    const auto registered = registerPlug("SN-1", "again");

    ASSERT_TRUE(registered.ok());
    EXPECT_TRUE(registered.value->restored);
    EXPECT_EQ(latestLogs(database, "SN-1").value->size(), 1u);
    EXPECT_EQ(finalized().back(), (std::pair<std::string, bool>{"SN-1", true}));
}

// 경계: 숨긴 지 89일 23:59:59면 복구, 정확히 90일이면 지난 기록을 지우고 새 기기
TEST_F(RegisterCommissionedTest, RestorePeriodBoundaryAt90Days) {
    hide("JUST_IN", kNow - kRestorePeriod + seconds(1));
    hide("EXPIRED", kNow - kRestorePeriod);

    const auto restored = registerPlug("JUST_IN");
    const auto fresh = registerPlug("EXPIRED");

    ASSERT_TRUE(restored.ok());
    EXPECT_TRUE(restored.value->restored);
    EXPECT_EQ(latestLogs(database, "JUST_IN").value->size(), 1u);
    ASSERT_TRUE(fresh.ok());
    EXPECT_FALSE(fresh.value->restored);
    EXPECT_TRUE(latestLogs(database, "EXPIRED").value->empty());
    EXPECT_EQ(database.findDevice("EXPIRED").value->kind, DeviceRecordState::Kind::Active);
}

TEST_F(RegisterCommissionedTest, RestorePeriodHelperKeepsRecordsWhenTimeIsUnreadable) {
    EXPECT_TRUE(DeviceManager::isWithinRestorePeriod("broken", kNow));
    EXPECT_TRUE(DeviceManager::isWithinRestorePeriod(textAt(kNow - kRestorePeriod + seconds(1)), kNow));
    EXPECT_FALSE(DeviceManager::isWithinRestorePeriod(textAt(kNow - kRestorePeriod), kNow));
}

TEST_F(RegisterCommissionedTest, DatabaseFailureIsInternalAndDriverIsToldToCleanUp) {
    database.close();

    const auto registered = registerPlug("SN-NEW");

    ASSERT_FALSE(registered.ok());
    EXPECT_EQ(registered.error.code, ErrorCode::Internal);
    EXPECT_TRUE(manager.activeDeviceIds().empty());
    EXPECT_EQ(finalized(), (std::vector<std::pair<std::string, bool>>{{"SN-NEW", false}}));
}

TEST_F(RegisterCommissionedTest, EmptyIdFailsAndShutdownCancels) {
    const auto empty = registerPlug("");
    EXPECT_EQ(empty.error.code, ErrorCode::CommissionFailed);

    manager.beginShutdown();
    const auto cancelled = registerPlug("SN-NEW");
    EXPECT_EQ(cancelled.error.code, ErrorCode::Cancelled);
    EXPECT_EQ(database.findDevice("SN-NEW").value->kind, DeviceRecordState::Kind::None);
    EXPECT_EQ(finalized(), (std::vector<std::pair<std::string, bool>>{{"", false}, {"SN-NEW", false}}));
}

// 확정 훅의 예외는 등록 결과를 바꾸지 않는다(서버를 죽이지 않음)
TEST_F(RegisterCommissionedTest, FinalizeExceptionDoesNotUndoRegistration) {
    matterFake.throwOnFinalize = true;
    const auto registered = registerPlug("SN-NEW");
    ASSERT_TRUE(registered.ok());
    EXPECT_EQ(manager.activeDeviceIds().count("SN-NEW"), 1u);
}

// M4: 같은 ID 삭제의 unpair가 늦게 끝나도, 재등록은 그 뒤에 판정·저장·확정한다
TEST_F(RegisterCommissionedTest, RegistrationWaitsForSlowUnpairOfSameId) {
    ASSERT_TRUE(manager.addDevice("SN-1", "plug", ProtocolType::MATTER, {}));
    Gate unpairStarted;
    Gate releaseUnpair;
    std::mutex orderMutex;
    std::vector<std::string> order;
    matterFake.onUnpair = [&] {
        unpairStarted.open();
        releaseUnpair.wait();
        std::lock_guard<std::mutex> lock(orderMutex);
        order.push_back("unpair");
    };
    matterFake.onFinalize = [&](const CommissionOutcome&, bool accepted) {
        std::lock_guard<std::mutex> lock(orderMutex);
        order.push_back(accepted ? "finalize-accepted" : "finalize-declined");
    };

    std::thread remover([&] { EXPECT_TRUE(manager.removeDevice("SN-1").ok()); });
    unpairStarted.wait();
    Gate registerDone;
    Result<RegisterOutcome> registered;
    std::thread registrar([&] {
        registered = registerPlug("SN-1");
        registerDone.open();
    });
    EXPECT_FALSE(registerDone.waitFor(std::chrono::milliseconds(100))) << "unpair가 끝나기 전에 등록하면 안 된다";
    releaseUnpair.open();
    remover.join();
    registrar.join();

    ASSERT_TRUE(registered.ok());
    EXPECT_TRUE(registered.value->restored);
    EXPECT_EQ(order, (std::vector<std::string>{"unpair", "finalize-accepted"}));
    EXPECT_EQ(manager.activeDeviceIds().count("SN-1"), 1u);
}

// 다른 ID의 삭제는 등록을 막지 않는다(ID별 락)
TEST_F(RegisterCommissionedTest, SlowUnpairOfOtherIdDoesNotBlockRegistration) {
    ASSERT_TRUE(manager.addDevice("SN-1", "plug", ProtocolType::MATTER, {}));
    Gate unpairStarted;
    Gate releaseUnpair;
    matterFake.onUnpair = [&] {
        unpairStarted.open();
        releaseUnpair.wait();
    };
    std::thread remover([&] { manager.removeDevice("SN-1"); });
    unpairStarted.wait();

    EXPECT_TRUE(registerPlug("SN-2").ok());

    releaseUnpair.open();
    remover.join();
}

// TSan: 같은 ID의 삭제·등록을 여러 번 동시에 돌려도 경합·교착 없이 끝나고, 메모리와 DB가 맞는다
TEST_F(RegisterCommissionedTest, ConcurrentRemoveAndRegisterOfSameIdStayConsistent) {
    ASSERT_TRUE(manager.addDevice("SN-1", "plug", ProtocolType::MATTER, {}));
    std::atomic<bool> stop{false};
    std::thread remover([&] {
        while (!stop) manager.removeDevice("SN-1");
    });
    for (int i = 0; i < 100; ++i) registerPlug("SN-1");
    stop = true;
    remover.join();

    const bool inMemory = manager.activeDeviceIds().count("SN-1") > 0;
    const auto kind = database.findDevice("SN-1").value->kind;
    EXPECT_EQ(inMemory, kind == DeviceRecordState::Kind::Active);
}
