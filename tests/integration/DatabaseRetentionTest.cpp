#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <string>
#include "DatabaseManager.hpp"
#include "UtcTime.hpp"
#include "support/HistoryTestHelper.hpp"

namespace {

using std::chrono::hours;
using std::chrono::seconds;

constexpr int kRetentionDays = 90;

// 시험에서 "지금"으로 쓰는 고정 시각
const std::chrono::system_clock::time_point kNow = *UtcTime::fromDbText("2026-11-21 00:00:00");
const std::chrono::system_clock::time_point kCutoff = kNow - hours(24 * kRetentionDays);

std::string textAt(std::chrono::system_clock::time_point time) { return UtcTime::toDbText(time); }

class DatabaseRetentionTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(db.init(":memory:")); }

    void addDevice(const std::string& id) {
        Device device(id, id, ProtocolType::MATTER);
        device.state = "ON";
        ASSERT_TRUE(db.insertDevice(device));
    }
    void addLog(const std::string& id, int64_t total, std::chrono::system_clock::time_point time) {
        ASSERT_TRUE(db.insertEnergyLog(id, total, textAt(time)));
    }
    void addRecentLogs(const std::string& id, int count) {
        for (int i = 0; i < count; ++i) addLog(id, 1000 + i, kNow - seconds(60 * (i + 1)));
    }
    size_t logCount(const std::string& id) { return latestLogs(db, id, 1000).value->size(); }

    DatabaseManager db;
};

}  // namespace

TEST_F(DatabaseRetentionTest, HiddenDeviceIsNotLoadedAndSecondHideFails) {
    addDevice("A");
    addDevice("B");
    ASSERT_TRUE(db.markDeleted("A", textAt(kNow)).ok());

    const auto active = db.loadActiveDevices();
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].id, "B");
    EXPECT_EQ(db.markDeleted("A", textAt(kNow)).error.code, ErrorCode::DeviceNotFound);
    EXPECT_EQ(db.markDeleted("NOPE", textAt(kNow)).error.code, ErrorCode::DeviceNotFound);
}

// 숨긴 기기를 같은 ID로 다시 등록하면 활성으로 돌아오고 통계·이력이 이어진다
TEST_F(DatabaseRetentionTest, ReRegisteringHiddenDeviceRestoresItWithStatsAndHistory) {
    addDevice("A");
    int64_t total = 0;
    ASSERT_TRUE(db.updateEnergyStat("A", 100, total));
    addLog("A", 100, kNow - hours(2));
    ASSERT_TRUE(db.markDeleted("A", textAt(kNow - hours(1))).ok());
    ASSERT_TRUE(db.loadActiveDevices().empty());

    addDevice("A");

    ASSERT_EQ(db.loadActiveDevices().size(), 1u);
    ASSERT_TRUE(db.updateEnergyStat("A", 150, total));
    EXPECT_EQ(total, 150); // 100 + 차이 50
    EXPECT_EQ(logCount("A"), 1u);
}

TEST_F(DatabaseRetentionTest, LogRetentionBoundaryAt90Days) {
    addDevice("A");
    addLog("A", 1, kCutoff - seconds(1)); // 90일을 막 넘김 -> 삭제
    addLog("A", 2, kCutoff);              // 정확히 기준 시각 -> 유지
    addLog("A", 3, kCutoff + seconds(1)); // 89일 23시간 59분 59초 -> 유지
    addRecentLogs("A", 10);

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_FALSE(purge.value->skipped);
    EXPECT_EQ(purge.value->deletedLogs, 1);
    const auto logs = *latestLogs(db, "A", 1000).value;
    ASSERT_EQ(logs.size(), 12u);
    EXPECT_EQ(logs.front().total_mwh, 2);
}

// 숨긴 지 90일이 지난 기기는 통계·이력·행이 모두 사라지고, 89일이면 남는다
TEST_F(DatabaseRetentionTest, HiddenDeviceIsPurgedAfter90DaysButKeptBefore) {
    addDevice("OLD");
    addDevice("RECENT");
    addDevice("LIVE");
    int64_t total = 0;
    ASSERT_TRUE(db.updateEnergyStat("OLD", 10, total));
    ASSERT_TRUE(db.updateEnergyStat("RECENT", 20, total));
    addLog("OLD", 10, kNow - hours(24));
    addLog("RECENT", 20, kNow - hours(24));
    addRecentLogs("LIVE", 5);
    ASSERT_TRUE(db.markDeleted("OLD", textAt(kCutoff - seconds(1))).ok());
    ASSERT_TRUE(db.markDeleted("RECENT", textAt(kCutoff + seconds(1))).ok());

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_EQ(purge.value->deletedDevices, 1);
    EXPECT_EQ(logCount("OLD"), 0u);
    EXPECT_EQ(logCount("RECENT"), 1u);
    // OLD의 통계가 지워졌으면 같은 ID는 최초 등록처럼 시작한다
    ASSERT_TRUE(db.updateEnergyStat("OLD", 5, total));
    EXPECT_EQ(total, 5);
    ASSERT_TRUE(db.updateEnergyStat("RECENT", 30, total));
    EXPECT_EQ(total, 30); // 20 + 10
}

TEST_F(DatabaseRetentionTest, SkipsWhenClockIsFarAheadOfLatestLog) {
    addDevice("A");
    addLog("A", 1, kCutoff - hours(24 * 10));
    addLog("A", 2, kCutoff - hours(24));

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_TRUE(purge.value->skipped);
    EXPECT_FALSE(purge.value->reason.empty());
    EXPECT_EQ(logCount("A"), 2u);
}

TEST_F(DatabaseRetentionTest, SkipsWhenMoreThanHalfOfLogsWouldBeDeleted) {
    addDevice("A");
    for (int i = 0; i < 6; ++i) addLog("A", i, kCutoff - hours(1 + i));
    addRecentLogs("A", 4);

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_TRUE(purge.value->skipped);
    EXPECT_EQ(logCount("A"), 10u);
}

TEST_F(DatabaseRetentionTest, ExactlyHalfIsNotSkipped) {
    addDevice("A");
    for (int i = 0; i < 5; ++i) addLog("A", i, kCutoff - hours(1 + i));
    addRecentLogs("A", 5);

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_FALSE(purge.value->skipped);
    EXPECT_EQ(purge.value->deletedLogs, 5);
    EXPECT_EQ(logCount("A"), 5u);
}

TEST_F(DatabaseRetentionTest, SkipDoesNotDeleteHiddenDevicesEither) {
    addDevice("A");
    ASSERT_TRUE(db.markDeleted("A", textAt(kCutoff - hours(24))).ok());
    addLog("A", 1, kCutoff - hours(48)); // 최신 이력이 기준일보다 오래 -> 건너뜀

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_TRUE(purge.value->skipped);
    EXPECT_EQ(logCount("A"), 1u);
}

// 기기 행이 없는 통계·이력(고아)은 정리된다. 숨긴 기기의 행은 고아가 아니다
TEST_F(DatabaseRetentionTest, OrphanRowsArePurgedButHiddenDeviceRowsAreNot) {
    addDevice("LIVE");
    addDevice("HIDDEN");
    addRecentLogs("LIVE", 3);
    int64_t total = 0;
    ASSERT_TRUE(db.updateEnergyStat("GHOST", 7, total));
    addLog("GHOST", 7, kNow - hours(1));
    addLog("HIDDEN", 9, kNow - hours(1));
    ASSERT_TRUE(db.markDeleted("HIDDEN", textAt(kNow - hours(1))).ok());

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_EQ(purge.value->deletedOrphans, 2); // 통계 1 + 이력 1
    EXPECT_EQ(logCount("GHOST"), 0u);
    EXPECT_EQ(logCount("HIDDEN"), 1u);
    ASSERT_TRUE(db.updateEnergyStat("GHOST", 3, total));
    EXPECT_EQ(total, 3);
}

TEST_F(DatabaseRetentionTest, PurgeOnEmptyDatabaseDoesNothing) {
    const auto purge = db.purgeExpired(textAt(kCutoff));
    ASSERT_TRUE(purge.ok());
    EXPECT_FALSE(purge.value->skipped);
    EXPECT_EQ(purge.value->deletedLogs + purge.value->deletedDevices + purge.value->deletedOrphans, 0);
}

TEST_F(DatabaseRetentionTest, TruncatedIsSetWhenMoreThanLimitExist) {
    addDevice("A");
    for (int i = 1; i <= 5; ++i) addLog("A", i * 100, kNow - seconds(60 * (6 - i)));

    const auto exact = db.queryHistory("A", std::nullopt, std::nullopt, 5);
    ASSERT_TRUE(exact.ok());
    EXPECT_FALSE(exact.value->truncated);
    EXPECT_EQ(exact.value->logs.size(), 5u);

    const auto cut = db.queryHistory("A", std::nullopt, std::nullopt, 4);
    ASSERT_TRUE(cut.ok());
    EXPECT_TRUE(cut.value->truncated);
    ASSERT_EQ(cut.value->logs.size(), 4u);
    EXPECT_EQ(cut.value->logs.front().total_mwh, 200); // 가장 오래된 100이 잘림
    EXPECT_EQ(cut.value->logs.back().total_mwh, 500);
}

TEST_F(DatabaseRetentionTest, QueryRangeIsHalfOpenAndRejectsBadLimit) {
    addDevice("A");
    addLog("A", 1, kNow - seconds(2));
    addLog("A", 2, kNow - seconds(1));
    addLog("A", 3, kNow);

    const auto page = db.queryHistory("A", textAt(kNow - seconds(1)), textAt(kNow), 50);

    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value->logs.size(), 1u);
    EXPECT_EQ(page.value->logs[0].total_mwh, 2); // from 포함, to 미만
    EXPECT_EQ(db.queryHistory("A", std::nullopt, std::nullopt, 0).error.code, ErrorCode::InvalidRequest);
}

TEST_F(DatabaseRetentionTest, AggregationSamplesIncludeOneBeforeRange) {
    addDevice("A");
    addLog("A", 1, kNow - hours(3));
    addLog("A", 2, kNow - hours(2)); // 범위 시작 직전 1건
    addLog("A", 3, kNow - hours(1));
    addLog("A", 4, kNow);            // 범위 끝(미포함)

    const auto samples = db.querySamplesForAggregation("A", textAt(kNow - hours(1)), textAt(kNow));

    ASSERT_TRUE(samples.ok());
    ASSERT_EQ(samples.value->size(), 2u);
    EXPECT_EQ((*samples.value)[0].total_mwh, 2);
    EXPECT_EQ((*samples.value)[1].total_mwh, 3);
}

TEST_F(DatabaseRetentionTest, LatestHistoryTimeIsEmptyThenMax) {
    addDevice("A");
    const auto empty = db.latestHistoryTime();
    ASSERT_TRUE(empty.ok());
    EXPECT_FALSE(empty.value->has_value());

    addLog("A", 1, kNow - hours(1));
    addLog("A", 2, kNow);
    EXPECT_EQ(**db.latestHistoryTime().value, textAt(kNow));
}

// 이력이 없고 숨김 기기가 전부 기준일을 지났다면 시계를 믿을 근거가 없어 건너뛴다
TEST_F(DatabaseRetentionTest, SkipsWhenNoHistoryAndEveryHiddenDeviceIsPastCutoff) {
    addDevice("OLD");
    ASSERT_TRUE(db.markDeleted("OLD", textAt(kCutoff - hours(24))).ok());

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_TRUE(purge.value->skipped);
    EXPECT_FALSE(purge.value->reason.empty());
    EXPECT_EQ(purge.value->deletedDevices, 0);
}

// 이력이 없어도 최근에 숨긴 기기가 하나라도 있으면 시계가 맞다고 보고 오래된 숨김 기기를 지운다
TEST_F(DatabaseRetentionTest, NoHistoryButRecentlyHiddenDeviceLetsOldHiddenBePurged) {
    addDevice("OLD");
    addDevice("RECENT");
    ASSERT_TRUE(db.markDeleted("OLD", textAt(kCutoff - hours(24))).ok());
    ASSERT_TRUE(db.markDeleted("RECENT", textAt(kNow - hours(1))).ok());

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_FALSE(purge.value->skipped);
    EXPECT_EQ(purge.value->deletedDevices, 1);
}

// 건너뜀(50% 초과)이면 숨김 기기도 고아도 그대로 둔다
TEST_F(DatabaseRetentionTest, HalfRuleSkipLeavesHiddenDevicesAndOrphansUntouched) {
    addDevice("A");
    addDevice("HIDDEN");
    for (int i = 0; i < 8; ++i) addLog("A", i, kCutoff - hours(1 + i));
    addRecentLogs("A", 4);
    addLog("HIDDEN", 1, kNow - hours(1));
    ASSERT_TRUE(db.markDeleted("HIDDEN", textAt(kCutoff - hours(24))).ok());
    addLog("GHOST", 1, kNow - hours(1));

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_TRUE(purge.value->skipped);
    EXPECT_EQ(logCount("HIDDEN"), 1u);
    EXPECT_EQ(logCount("GHOST"), 1u);
    EXPECT_EQ(logCount("A"), 12u);
}

// 기준 시각과 정확히 같은 숨김 시각은 아직 지우지 않는다
TEST_F(DatabaseRetentionTest, HiddenAtExactlyCutoffIsKept) {
    addDevice("EDGE");
    addDevice("LIVE");
    addRecentLogs("LIVE", 3);
    addLog("EDGE", 1, kNow - hours(1));
    ASSERT_TRUE(db.markDeleted("EDGE", textAt(kCutoff)).ok());

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_EQ(purge.value->deletedDevices, 0);
    EXPECT_EQ(logCount("EDGE"), 1u);
}

// 고아 행은 50% 안전장치 대상이 아니다: 고아가 전체 이력의 대부분이어도 지워진다
TEST_F(DatabaseRetentionTest, OrphansAreNotCountedInHalfRule) {
    addDevice("LIVE");
    addRecentLogs("LIVE", 1);
    for (int i = 0; i < 10; ++i) addLog("GHOST", i, kNow - seconds(60 * (i + 1)));

    const auto purge = db.purgeExpired(textAt(kCutoff));

    ASSERT_TRUE(purge.ok());
    EXPECT_FALSE(purge.value->skipped);
    EXPECT_EQ(purge.value->deletedOrphans, 10);
    EXPECT_EQ(logCount("LIVE"), 1u);
}

// 정리 도중 실패하면 앞 단계에서 지운 것까지 모두 되돌린다
TEST(DatabasePurgeRollbackTest, MidwayFailureRollsBackEverything) {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / ("hac_purge_rollback_" + std::to_string(::getpid()) + ".db");
    fs::remove(path);
    DatabaseManager db;
    ASSERT_TRUE(db.init(path.string()));
    Device live("LIVE", "LIVE", ProtocolType::MATTER);
    Device old("OLD", "OLD", ProtocolType::MATTER);
    ASSERT_TRUE(db.insertDevice(live));
    ASSERT_TRUE(db.insertDevice(old));
    ASSERT_TRUE(db.insertEnergyLog("LIVE", 1, textAt(kCutoff - hours(1)))); // 지울 이력(전체의 1/4)
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(db.insertEnergyLog("LIVE", 2 + i, textAt(kNow - seconds(60 * (i + 1)))));
    ASSERT_TRUE(db.markDeleted("OLD", textAt(kCutoff - hours(24))).ok());
    ASSERT_TRUE(db.insertEnergyLog("OLD", 9, textAt(kNow - hours(1))));

    // 같은 파일을 직접 열어 기기 행 삭제를 막는 트리거를 건다(마지막 단계에서 실패)
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "CREATE TRIGGER block_purge BEFORE DELETE ON devices "
                                "BEGIN SELECT RAISE(ABORT, 'blocked'); END;", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);

    const auto purge = db.purgeExpired(textAt(kCutoff));

    EXPECT_FALSE(purge.ok());
    EXPECT_EQ(latestLogs(db, "LIVE", 1000).value->size(), 4u); // 오래된 이력 삭제도 취소
    EXPECT_EQ(latestLogs(db, "OLD", 1000).value->size(), 1u);
    db.close();
    fs::remove(path);
}

TEST_F(DatabaseRetentionTest, FindDeviceTellsNoneActiveAndHiddenWithTime) {
    addDevice("A");
    addDevice("B");
    ASSERT_TRUE(db.markDeleted("B", textAt(kNow - hours(3))).ok());

    const auto none = db.findDevice("NOPE");
    const auto active = db.findDevice("A");
    const auto hidden = db.findDevice("B");

    ASSERT_TRUE(none.ok());
    EXPECT_EQ(none.value->kind, DeviceRecordState::Kind::None);
    ASSERT_TRUE(active.ok());
    EXPECT_EQ(active.value->kind, DeviceRecordState::Kind::Active);
    EXPECT_TRUE(active.value->deletedAtUtc.empty());
    ASSERT_TRUE(hidden.ok());
    EXPECT_EQ(hidden.value->kind, DeviceRecordState::Kind::Hidden);
    EXPECT_EQ(hidden.value->deletedAtUtc, textAt(kNow - hours(3)));
}

TEST_F(DatabaseRetentionTest, FindDeviceFailsWhenDatabaseIsClosed) {
    db.close();
    const auto found = db.findDevice("A");
    EXPECT_FALSE(found.ok());
    EXPECT_EQ(found.error.code, ErrorCode::Internal);
}

// 숨김 행 하나의 기록만 지우고 다른 기기(숨김 포함)는 건드리지 않는다
TEST_F(DatabaseRetentionTest, PurgeDeviceRecordsRemovesOnlyThatHiddenDevice) {
    addDevice("OLD");
    addDevice("OTHER_HIDDEN");
    addDevice("LIVE");
    int64_t total = 0;
    for (const char* id : {"OLD", "OTHER_HIDDEN", "LIVE"}) {
        ASSERT_TRUE(db.updateEnergyStat(id, 100, total));
        addLog(id, 100, kNow - hours(1));
    }
    ASSERT_TRUE(db.markDeleted("OLD", textAt(kCutoff - hours(1))).ok());
    ASSERT_TRUE(db.markDeleted("OTHER_HIDDEN", textAt(kCutoff - hours(1))).ok());

    ASSERT_TRUE(db.purgeDeviceRecords("OLD").ok());

    EXPECT_EQ(db.findDevice("OLD").value->kind, DeviceRecordState::Kind::None);
    EXPECT_EQ(logCount("OLD"), 0u);
    EXPECT_EQ(db.findDevice("OTHER_HIDDEN").value->kind, DeviceRecordState::Kind::Hidden);
    EXPECT_EQ(logCount("OTHER_HIDDEN"), 1u);
    EXPECT_EQ(logCount("LIVE"), 1u);
    // OLD 통계가 지워졌으면 새로 등록한 기기는 0에서 시작한다
    addDevice("OLD");
    ASSERT_TRUE(db.updateEnergyStat("OLD", 7, total));
    EXPECT_EQ(total, 7);
    ASSERT_TRUE(db.updateEnergyStat("LIVE", 150, total));
    EXPECT_EQ(total, 150);
}

TEST_F(DatabaseRetentionTest, PurgeDeviceRecordsRefusesActiveOrMissingDevice) {
    addDevice("LIVE");
    addLog("LIVE", 1, kNow - hours(1));

    EXPECT_EQ(db.purgeDeviceRecords("LIVE").error.code, ErrorCode::DeviceNotFound);
    EXPECT_EQ(db.purgeDeviceRecords("NOPE").error.code, ErrorCode::DeviceNotFound);
    EXPECT_EQ(db.findDevice("LIVE").value->kind, DeviceRecordState::Kind::Active);
    EXPECT_EQ(logCount("LIVE"), 1u);
}

TEST_F(DatabaseRetentionTest, PurgeDeviceRecordsFailsWhenDatabaseIsClosed) {
    db.close();
    EXPECT_EQ(db.purgeDeviceRecords("A").error.code, ErrorCode::Internal);
}

// Q13: 기기 행 삭제가 중간에 실패하면 앞서 지운 통계·이력도 되돌린다(한 트랜잭션)
TEST(DatabaseRetentionFileTest, PurgeDeviceRecordsRollsBackWhenDeviceDeleteFails) {
    const std::string path = (std::filesystem::temp_directory_path() /
                              ("hac_purge_rollback_" + std::to_string(::getpid()) + ".db")).string();
    std::filesystem::remove(path);
    DatabaseManager db;
    ASSERT_TRUE(db.init(path));
    Device device("OLD", "old", ProtocolType::MATTER);
    ASSERT_TRUE(db.insertDevice(device));
    int64_t total = 0;
    ASSERT_TRUE(db.updateEnergyStat("OLD", 100, total));
    ASSERT_TRUE(db.insertEnergyLog("OLD", 100, textAt(kNow - hours(1))));
    ASSERT_TRUE(db.markDeleted("OLD", textAt(kCutoff - hours(1))).ok());
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw, "CREATE TRIGGER keep_device BEFORE DELETE ON devices BEGIN SELECT RAISE(ABORT, 'no'); END;",
                               nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close_v2(raw);
    }

    EXPECT_EQ(db.purgeDeviceRecords("OLD").error.code, ErrorCode::Internal);

    EXPECT_EQ(db.findDevice("OLD").value->kind, DeviceRecordState::Kind::Hidden);
    EXPECT_EQ(latestLogs(db, "OLD", 10).value->size(), 1u);
    ASSERT_TRUE(db.updateEnergyStat("OLD", 150, total));
    EXPECT_EQ(total, 150);  // 통계가 남아 있으면 100 + 50
    db.close();
    std::filesystem::remove(path);
}
