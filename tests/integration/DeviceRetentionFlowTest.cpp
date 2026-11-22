#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>
#include <filesystem>
#include <string>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "UtcTime.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "support/HistoryTestHelper.hpp"

namespace fs = std::filesystem;
using std::chrono::hours;

namespace {

const std::chrono::system_clock::time_point kStart = *UtcTime::fromDbText("2026-11-01 00:00:00");

// 같은 파일을 직접 여는 검증용 연결(트리거로 DB 실패를 만들거나 행 수를 센다)
class RawConnection {
public:
    explicit RawConnection(const fs::path& path) { EXPECT_EQ(sqlite3_open(path.c_str(), &db_), SQLITE_OK); }
    ~RawConnection() { sqlite3_close(db_); }
    void exec(const std::string& sql) { EXPECT_EQ(sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK); }
    int64_t count(const std::string& sql) {
        sqlite3_stmt* stmt = nullptr;
        int64_t value = -1;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            value = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return value;
    }
private:
    sqlite3* db_ = nullptr;
};

class DeviceRetentionFlowTest : public ::testing::Test {
protected:
    void SetUp() override {
        dbPath_ = fs::temp_directory_path() / ("hac_retention_" + std::to_string(::getpid()) + ".db");
        fs::remove(dbPath_);
        ASSERT_TRUE(database.init(dbPath_.string()));
        manager.registerDriver(ProtocolType::MATTER, &matterFake);
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    }
    void TearDown() override {
        database.close();
        fs::remove(dbPath_);
    }
    void addPlug(const std::string& id) {
        ASSERT_TRUE(manager.addDevice(id, "plug", ProtocolType::MATTER, {}, &matterFake));
    }
    std::string nowText() { return UtcTime::toDbText(clock.utcNow()); }

    fs::path dbPath_;
    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock{{}, kStart};
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver matterFake;
    FakeProtocolDriver tcpFake;
};

}  // namespace

TEST_F(DeviceRetentionFlowTest, HiddenDeviceIsNotFoundEverywhereThenReRegistrationKeepsStatsAndHistory) {
    addPlug("PLUG");
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1000, 5000});
    ASSERT_TRUE(manager.readEnergy("PLUG").ok());
    manager.runHealthCheck(); // 이력 1건(가짜 시계 시각)

    const auto outcome = manager.removeDevice("PLUG");

    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.value->deletedAtUtc, "2026-11-01 00:00:00");
    EXPECT_TRUE(outcome.value->unpaired);
    EXPECT_TRUE(manager.getAllDevices().empty());
    EXPECT_EQ(manager.refreshState("PLUG", false).error.code, ErrorCode::DeviceNotFound);
    EXPECT_EQ(manager.readEnergy("PLUG").error.code, ErrorCode::DeviceNotFound);
    EXPECT_EQ(manager.getEnergyHistory("PLUG", HistoryQuery{}).error.code, ErrorCode::DeviceNotFound);
    EXPECT_EQ(manager.removeDevice("PLUG").error.code, ErrorCode::DeviceNotFound);

    clock.advance(hours(24));
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"PLUG"});
    ASSERT_TRUE(manager.startCommissioning(ProtocolType::MATTER, "plug again", ""));

    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1000, 5300});
    const auto energy = manager.readEnergy("PLUG");
    ASSERT_TRUE(energy.ok());
    EXPECT_EQ(energy.value->totalEnergy_mWh, 5300); // 누적이 0으로 돌아가지 않고 이어진다
    const auto history = manager.getEnergyHistory("PLUG", HistoryQuery{});
    ASSERT_TRUE(history.ok());
    EXPECT_EQ(history.value->logs.size(), 1u);
}

TEST_F(DeviceRetentionFlowTest, UnpairFailureReportsFalseButKeepsDeviceHidden) {
    addPlug("PLUG");
    matterFake.unpairResult = false;

    const auto outcome = manager.removeDevice("PLUG");

    ASSERT_TRUE(outcome.ok());
    EXPECT_FALSE(outcome.value->unpaired);
    EXPECT_TRUE(database.loadActiveDevices().empty());
}

// 동시 추가·삭제 뒤 "메모리에만 남고 DB는 이미 숨김"인 기기도 지울 수 있어야 한다(되돌리면 영영 못 지움)
TEST_F(DeviceRetentionFlowTest, AlreadyHiddenInDatabaseStillRemovesFromMemory) {
    addPlug("PLUG");
    ASSERT_TRUE(database.markDeleted("PLUG", nowText()).ok()); // 메모리는 그대로, DB만 숨김

    const auto outcome = manager.removeDevice("PLUG");

    ASSERT_TRUE(outcome.ok());
    EXPECT_TRUE(outcome.value->unpaired);
    EXPECT_TRUE(manager.getAllDevices().empty());
    EXPECT_EQ(matterFake.unpairedIds.size(), 1u);
    EXPECT_TRUE(database.loadActiveDevices().empty());
}

TEST_F(DeviceRetentionFlowTest, DatabaseFailureRestoresDeviceAndReturnsInternal) {
    addPlug("PLUG");
    RawConnection raw(dbPath_);
    raw.exec("CREATE TRIGGER block_hide BEFORE UPDATE OF deleted_at ON devices "
             "BEGIN SELECT RAISE(ABORT, 'blocked'); END;");

    const auto outcome = manager.removeDevice("PLUG");

    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.error.code, ErrorCode::Internal);
    EXPECT_EQ(manager.getAllDevices().size(), 1u); // 맵에 되돌아와 있다
    EXPECT_TRUE(matterFake.unpairedIds.empty());   // 숨김이 안 됐으니 기기 쪽도 건드리지 않는다
    EXPECT_EQ(database.loadActiveDevices().size(), 1u);
}

TEST_F(DeviceRetentionFlowTest, TcpDeviceHistoryIsUnsupported) {
    ASSERT_TRUE(manager.addDevice("FAKE_1", "lamp", ProtocolType::TCP_DIY, {}, &tcpFake));

    EXPECT_EQ(manager.getEnergyHistory("FAKE_1", HistoryQuery{}).error.code, ErrorCode::UnsupportedOperation);
    EXPECT_EQ(manager.readEnergy("FAKE_1").error.code, ErrorCode::UnsupportedOperation);
}

TEST_F(DeviceRetentionFlowTest, HealthCheckRecordsFakeClockTime) {
    addPlug("PLUG");

    manager.runHealthCheck();

    const auto logs = latestLogs(database, "PLUG");
    ASSERT_TRUE(logs.ok());
    ASSERT_EQ(logs.value->size(), 1u);
    EXPECT_EQ((*logs.value)[0].timestamp, "2026-11-01 00:00:00");
}

TEST_F(DeviceRetentionFlowTest, HistoryQueryAppliesLocalDateRangeAndAggregation) {
    addPlug("PLUG");
    // 현지 11-01 23:59:59(UTC 14:59:59)와 11-02 00:00:00(UTC 15:00:00)
    ASSERT_TRUE(database.insertEnergyLog("PLUG", 100, "2026-11-01 14:59:59"));
    ASSERT_TRUE(database.insertEnergyLog("PLUG", 160, "2026-11-01 15:00:00"));

    HistoryQuery raw;
    raw.from = CivilDate{2026, 11, 2};
    raw.to = CivilDate{2026, 11, 2};
    const auto page = manager.getEnergyHistory("PLUG", raw);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value->logs.size(), 1u);
    EXPECT_EQ(page.value->logs[0].total_mwh, 160);
    EXPECT_FALSE(page.value->truncated);

    HistoryQuery weekly = raw;
    weekly.from = CivilDate{2026, 11, 1};
    weekly.period = HistoryPeriod::Week;
    const auto aggregated = manager.getEnergyHistory("PLUG", weekly);
    ASSERT_TRUE(aggregated.ok());
    ASSERT_EQ(aggregated.value->buckets.size(), 2u);
    EXPECT_EQ(aggregated.value->buckets[0].sampleCount, 1); // 일요일 하루
    EXPECT_EQ(aggregated.value->buckets[1].energyUsed_mWh, 60);
}

TEST_F(DeviceRetentionFlowTest, CleanupWithFakeClockHonors90DayBoundary) {
    addPlug("PLUG");
    addPlug("LIVE");
    ASSERT_TRUE(database.insertEnergyLog("PLUG", 1, nowText()));
    ASSERT_TRUE(manager.removeDevice("PLUG").ok()); // 숨김 시각 = kStart
    RawConnection raw(dbPath_);

    clock.advance(hours(24 * 90) - std::chrono::seconds(1)); // 89일 23시간 59분 59초
    ASSERT_TRUE(database.insertEnergyLog("LIVE", 2, nowText()));
    ASSERT_TRUE(manager.runRetentionCleanup().ok());
    EXPECT_EQ(raw.count("SELECT COUNT(*) FROM devices WHERE device_id = 'PLUG'"), 1);
    EXPECT_EQ(raw.count("SELECT COUNT(*) FROM state_history_logs WHERE device_id = 'PLUG'"), 1);

    clock.advance(std::chrono::seconds(2)); // 90일을 막 넘김
    ASSERT_TRUE(database.insertEnergyLog("LIVE", 3, nowText()));
    const auto cleanup = manager.runRetentionCleanup();
    ASSERT_TRUE(cleanup.ok());
    EXPECT_FALSE(cleanup.value->skipped);
    EXPECT_EQ(cleanup.value->deletedDevices, 1);
    EXPECT_EQ(raw.count("SELECT COUNT(*) FROM devices WHERE device_id = 'PLUG'"), 0);
    EXPECT_EQ(raw.count("SELECT COUNT(*) FROM state_history_logs WHERE device_id = 'PLUG'"), 0);
    EXPECT_EQ(raw.count("SELECT COUNT(*) FROM devices WHERE device_id = 'LIVE'"), 1);
}
