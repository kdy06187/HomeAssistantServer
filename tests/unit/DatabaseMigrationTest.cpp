#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/stat.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "DatabaseManager.hpp"
#include "support/HistoryTestHelper.hpp"

namespace fs = std::filesystem;

namespace {

// 검증용 별도 연결. DatabaseManager를 거치지 않고 파일을 직접 읽는다.
class RawDb {
public:
    explicit RawDb(const fs::path& path, int flags = SQLITE_OPEN_READWRITE) {
        EXPECT_EQ(sqlite3_open_v2(path.c_str(), &db_, flags, nullptr), SQLITE_OK);
    }
    ~RawDb() { sqlite3_close(db_); }

    void exec(const std::string& sql) {
        char* err = nullptr;
        EXPECT_EQ(sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
        sqlite3_free(err);
    }

    // 첫 행 첫 열을 문자열로 반환. NULL이면 "<null>", 행이 없으면 "<none>"
    std::string scalar(const std::string& sql) {
        sqlite3_stmt* stmt = nullptr;
        std::string out = "<none>";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            out = text ? reinterpret_cast<const char*>(text) : "<null>";
        }
        sqlite3_finalize(stmt);
        return out;
    }

private:
    sqlite3* db_ = nullptr;
};

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class DatabaseMigrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               (std::string("hac_db_test_") + std::to_string(::getpid()) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        dbPath_ = (dir_ / "test.db").string();
    }
    void TearDown() override { fs::remove_all(dir_); }

    void createV0Db() {
        RawDb raw(dbPath_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        raw.exec(readFile(HAC_TEST_V0_SCHEMA_SQL));
    }

    std::vector<fs::path> backups() {
        std::vector<fs::path> found;
        for (const auto& entry : fs::directory_iterator(dir_)) {
            if (entry.path().filename().string().rfind("test.db.bak-v", 0) == 0) found.push_back(entry.path());
        }
        return found;
    }

    fs::path dir_;
    std::string dbPath_;
};

TEST_F(DatabaseMigrationTest, MigratesV0ToV1AndKeepsData) {
    createV0Db();
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_, "20260101T000000Z"));
    db.close();

    RawDb raw(dbPath_);
    EXPECT_EQ(raw.scalar("PRAGMA user_version"), "1");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM devices"), "3");
    EXPECT_EQ(raw.scalar("SELECT display_name FROM devices WHERE device_id='SN-MATTER-1'"), "Living Plug");
    EXPECT_EQ(raw.scalar("SELECT real_total_mwh FROM device_energy_stats WHERE device_id='SN-MATTER-1'"), "5000");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM state_history_logs"), "3");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM pragma_table_info('devices') WHERE name IN ('ip','deleted_at')"), "2");
    EXPECT_EQ(raw.scalar("SELECT name FROM sqlite_master WHERE type='index' AND name='idx_history_device_time'"),
              "idx_history_device_time");
    EXPECT_EQ(raw.scalar("SELECT ip FROM devices WHERE device_id='ARD_192.168.0.10'"), "192.168.0.10");
    EXPECT_EQ(raw.scalar("SELECT ip FROM devices WHERE device_id='LEGACY_X'"), "<null>");
    EXPECT_EQ(raw.scalar("SELECT ip FROM devices WHERE device_id='SN-MATTER-1'"), "<null>");
}

TEST_F(DatabaseMigrationTest, CreatesSecureBackupOfV0) {
    createV0Db();
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_, "20260101T000000Z"));
    db.close();

    auto files = backups();
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].filename().string(), "test.db.bak-v0-20260101T000000Z");

    struct stat st{};
    ASSERT_EQ(::stat(files[0].c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);

    RawDb backup(files[0], SQLITE_OPEN_READONLY);
    EXPECT_EQ(backup.scalar("PRAGMA user_version"), "0");
    EXPECT_EQ(backup.scalar("SELECT COUNT(*) FROM devices"), "3");
    EXPECT_EQ(backup.scalar("SELECT COUNT(*) FROM state_history_logs"), "3");
}

TEST_F(DatabaseMigrationTest, AlreadyV1MakesNoBackup) {
    createV0Db();
    {
        DatabaseManager db;
        ASSERT_TRUE(db.init(dbPath_, "20260101T000000Z"));
    }
    fs::remove(backups()[0]);

    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_, "20260102T000000Z"));
    db.close();

    EXPECT_TRUE(backups().empty());
    RawDb raw(dbPath_);
    EXPECT_EQ(raw.scalar("PRAGMA user_version"), "1");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM devices"), "3");
}

TEST_F(DatabaseMigrationTest, NewFileReachesV1WithoutBackup) {
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_));
    db.close();

    EXPECT_TRUE(backups().empty());
    RawDb raw(dbPath_);
    EXPECT_EQ(raw.scalar("PRAGMA user_version"), "1");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM pragma_table_info('devices') WHERE name='ip'"), "1");
}

TEST_F(DatabaseMigrationTest, InMemoryDbMigratesWithoutBackup) {
    DatabaseManager db;
    EXPECT_TRUE(db.init(":memory:"));
}

TEST_F(DatabaseMigrationTest, KeepsOnlyThreeNewestBackups) {
    createV0Db();
    for (const char* stamp : {"20250101T000000Z", "20250102T000000Z", "20250103T000000Z", "20250104T000000Z"}) {
        std::ofstream(dbPath_ + ".bak-v0-" + stamp) << "old";
    }
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_, "20260101T000000Z"));
    db.close();

    auto files = backups();
    ASSERT_EQ(files.size(), 3u);
    std::vector<std::string> names;
    for (const auto& f : files) names.push_back(f.filename().string());
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "test.db.bak-v0-20250103T000000Z");
    EXPECT_EQ(names[1], "test.db.bak-v0-20250104T000000Z");
    EXPECT_EQ(names[2], "test.db.bak-v0-20260101T000000Z");
}

// 시계가 뒤로 가서 기존 백업이 더 "최근"으로 보여도 방금 만든 백업은 남아야 한다
TEST_F(DatabaseMigrationTest, KeepsFreshBackupEvenIfOlderStampsLookNewer) {
    createV0Db();
    for (const char* stamp : {"20300101T000000Z", "20300102T000000Z", "20300103T000000Z"}) {
        std::ofstream(dbPath_ + ".bak-v0-" + stamp) << "future";
    }
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_, "20260101T000000Z"));
    db.close();

    std::vector<std::string> names;
    for (const auto& f : backups()) names.push_back(f.filename().string());
    std::sort(names.begin(), names.end());
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "test.db.bak-v0-20260101T000000Z");
    EXPECT_EQ(names[1], "test.db.bak-v0-20300102T000000Z");
    EXPECT_EQ(names[2], "test.db.bak-v0-20300103T000000Z");
}

TEST_F(DatabaseMigrationTest, ReinitClosesPreviousConnection) {
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_));
    ASSERT_TRUE(db.init(dbPath_));
    db.close();
}

TEST_F(DatabaseMigrationTest, FailedMigrationRollsBackAndReturnsFalse) {
    createV0Db();
    {
        RawDb raw(dbPath_);
        raw.exec("ALTER TABLE devices ADD COLUMN ip TEXT");  // v1의 첫 ALTER가 중복되어 실패한다
    }
    DatabaseManager db;
    EXPECT_FALSE(db.init(dbPath_, "20260101T000000Z"));

    RawDb raw(dbPath_);
    EXPECT_EQ(raw.scalar("PRAGMA user_version"), "0");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM pragma_table_info('devices') WHERE name='deleted_at'"), "0");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM devices"), "3");
}

TEST_F(DatabaseMigrationTest, InsertDeviceExecutesInsertOnce) {
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_));
    {
        // 별도 연결에서 INSERT 횟수를 세는 트리거를 건다
        RawDb raw(dbPath_);
        raw.exec("CREATE TABLE insert_audit(n INTEGER);"
                 "CREATE TRIGGER audit_insert AFTER INSERT ON devices BEGIN INSERT INTO insert_audit VALUES (1); END;");
    }
    Device device("ARD_10.0.0.5", "Fan", ProtocolType::TCP_DIY);
    device.state = "ON";
    ASSERT_TRUE(db.insertDevice(device));

    RawDb raw(dbPath_);
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM insert_audit"), "1");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM devices"), "1");
}

TEST_F(DatabaseMigrationTest, DeviceCrudRoundTrip) {
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_));
    Device device("SN-1", "Plug", ProtocolType::MATTER);
    device.state = "ON";
    ASSERT_TRUE(db.insertDevice(device));

    auto loaded = db.loadActiveDevices();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id, "SN-1");
    EXPECT_EQ(loaded[0].name, "Plug");
    EXPECT_EQ(loaded[0].state, "ON");

    ASSERT_TRUE(db.updateDeviceState("SN-1", 0));
    EXPECT_EQ(db.loadActiveDevices()[0].state, "OFF");

    ASSERT_TRUE(db.markDeleted("SN-1", "2026-11-19 00:00:00").ok());
    EXPECT_TRUE(db.loadActiveDevices().empty());
}

TEST_F(DatabaseMigrationTest, EnergyHistoryReturnsOldestFirstWithLimit) {
    createV0Db();
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_, "20260101T000000Z"));

    auto all = latestLogs(db, "SN-MATTER-1");
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all.value->size(), 3u);
    EXPECT_EQ((*all.value)[0].total_mwh, 100);
    EXPECT_EQ((*all.value)[2].total_mwh, 300);

    auto latestTwo = latestLogs(db, "SN-MATTER-1", 2);
    ASSERT_TRUE(latestTwo.ok());
    ASSERT_EQ(latestTwo.value->size(), 2u);
    EXPECT_EQ((*latestTwo.value)[0].total_mwh, 200);
    auto none = latestLogs(db, "none");
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.value->empty());
}

TEST_F(DatabaseMigrationTest, EnergyStatAccumulatesDeltaAndHandlesReset) {
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_));
    int64_t total = 0;
    ASSERT_TRUE(db.updateEnergyStat("p", 100, total));
    EXPECT_EQ(total, 100);
    ASSERT_TRUE(db.updateEnergyStat("p", 150, total));
    EXPECT_EQ(total, 150);
    ASSERT_TRUE(db.updateEnergyStat("p", 30, total));  // 카운터 초기화: 새 값을 그대로 더한다
    EXPECT_EQ(total, 180);
}

TEST_F(DatabaseMigrationTest, ConcurrentEnergyUpdatesAreConsistent) {
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_));
    constexpr int kThreads = 8;
    constexpr int kCalls = 100;
    std::atomic<int> failures{0};

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            const std::string own = "dev" + std::to_string(t);
            for (int i = 1; i <= kCalls; ++i) {
                int64_t total = 0;
                if (!db.updateEnergyStat(own, i * 10, total)) ++failures;
                if (!db.updateEnergyStat("shared", 500, total)) ++failures;  // 같은 값 반복이면 증가분 0
            }
        });
    }
    for (auto& w : workers) w.join();

    EXPECT_EQ(failures.load(), 0);
    int64_t total = 0;
    for (int t = 0; t < kThreads; ++t) {
        ASSERT_TRUE(db.updateEnergyStat("dev" + std::to_string(t), kCalls * 10, total));
        EXPECT_EQ(total, kCalls * 10);
    }
    ASSERT_TRUE(db.updateEnergyStat("shared", 500, total));
    EXPECT_EQ(total, 500);
}

TEST_F(DatabaseMigrationTest, MidMigrationFailureRollsBackEarlierAlter) {
    createV0Db();
    {
        RawDb raw(dbPath_);
        raw.exec("ALTER TABLE devices ADD COLUMN deleted_at TEXT");  // 첫 ALTER(ip)는 성공, 두 번째가 실패한다
    }
    DatabaseManager db;
    EXPECT_FALSE(db.init(dbPath_, "20260101T000000Z"));

    RawDb raw(dbPath_);
    EXPECT_EQ(raw.scalar("PRAGMA user_version"), "0");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM pragma_table_info('devices') WHERE name='ip'"), "0");
}

TEST_F(DatabaseMigrationTest, BackupNameCollisionFailsAndKeepsDbUntouched) {
    createV0Db();
    std::ofstream(dbPath_ + ".bak-v0-20260101T000000Z") << "existing";
    DatabaseManager db;
    EXPECT_FALSE(db.init(dbPath_, "20260101T000000Z"));

    EXPECT_EQ(readFile(dbPath_ + ".bak-v0-20260101T000000Z"), "existing");
    RawDb raw(dbPath_);
    EXPECT_EQ(raw.scalar("PRAGMA user_version"), "0");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM pragma_table_info('devices') WHERE name='ip'"), "0");
}

TEST_F(DatabaseMigrationTest, InitFailsOnNonDatabaseFile) {
    std::ofstream(dbPath_) << "this is not a sqlite database, just text padding padding padding";
    DatabaseManager db;
    EXPECT_FALSE(db.init(dbPath_, "20260101T000000Z"));
    EXPECT_TRUE(backups().empty());
    EXPECT_TRUE(db.loadActiveDevices().empty());  // 실패 후 db_가 닫혀 있어도 죽지 않는다
}

TEST_F(DatabaseMigrationTest, PruneIgnoresBackupsOfOtherDbFiles) {
    createV0Db();
    for (const char* stamp : {"20240101T000000Z", "20240102T000000Z", "20240103T000000Z", "20240104T000000Z"}) {
        std::ofstream((dir_ / (std::string("other.db.bak-v0-") + stamp)).string()) << "x";
    }
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_, "20260101T000000Z"));
    db.close();

    size_t others = 0;
    for (const auto& e : fs::directory_iterator(dir_)) {
        if (e.path().filename().string().rfind("other.db.bak-v", 0) == 0) ++others;
    }
    EXPECT_EQ(others, 4u);
}

TEST_F(DatabaseMigrationTest, PruneOrdersByTimeAcrossVersionDigits) {
    createV0Db();
    std::ofstream(dbPath_ + ".bak-v9-20250101T000000Z") << "x";
    std::ofstream(dbPath_ + ".bak-v10-20250102T000000Z") << "x";
    std::ofstream(dbPath_ + ".bak-v10-20250103T000000Z") << "x";
    DatabaseManager db;
    ASSERT_TRUE(db.init(dbPath_, "20260101T000000Z"));
    db.close();

    EXPECT_FALSE(fs::exists(dbPath_ + ".bak-v9-20250101T000000Z"));  // 가장 오래된 것만 삭제
    EXPECT_TRUE(fs::exists(dbPath_ + ".bak-v10-20250102T000000Z"));
    EXPECT_TRUE(fs::exists(dbPath_ + ".bak-v0-20260101T000000Z"));
}

TEST_F(DatabaseMigrationTest, LegacySqlStillWorksOnV1Db) {
    createV0Db();
    {
        DatabaseManager db;
        ASSERT_TRUE(db.init(dbPath_, "20260101T000000Z"));
    }
    RawDb raw(dbPath_);
    // 옛 실행 파일이 쓰던 SQL 그대로(열 지정 INSERT OR REPLACE, 열 지정 SELECT, IF NOT EXISTS)
    raw.exec("CREATE TABLE IF NOT EXISTS devices(device_id TEXT PRIMARY KEY, display_name TEXT, "
             "protocol_type INTEGER, is_active INTEGER, driverTypes TEXT);"
             "CREATE TABLE IF NOT EXISTS device_energy_stats(device_id TEXT PRIMARY KEY, "
             "real_total_mwh BIGINT DEFAULT 0, last_plug_mwh BIGINT DEFAULT 0);"
             "CREATE TABLE IF NOT EXISTS state_history_logs(log_id INTEGER PRIMARY KEY AUTOINCREMENT, "
             "device_id TEXT, total_mwh BIGINT, timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP);"
             "INSERT OR REPLACE INTO devices (device_id, display_name, protocol_type, is_active, driverTypes) "
             "VALUES ('NEW-1', 'N', 0, 1, '1,2');"
             "UPDATE devices SET is_active = 0 WHERE device_id = 'NEW-1';"
             "INSERT OR REPLACE INTO device_energy_stats (device_id, real_total_mwh, last_plug_mwh) VALUES ('NEW-1', 1, 1);"
             "INSERT INTO state_history_logs (device_id, total_mwh) VALUES ('NEW-1', 7);"
             "DELETE FROM devices WHERE device_id = 'LEGACY_X';");
    EXPECT_EQ(raw.scalar("SELECT driverTypes FROM devices WHERE device_id='NEW-1'"), "1,2");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM devices"), "3");
    EXPECT_EQ(raw.scalar("PRAGMA user_version"), "1");
}

// 백업은 원본에 아무것도 쓰기 전에 한다: 백업이 실패하면 원본 파일이 바이트 단위로 그대로다
TEST_F(DatabaseMigrationTest, BackupFailureLeavesOriginalFileByteForByte) {
    {
        // 전력 테이블이 아직 없던 아주 옛 DB(기기 테이블만)
        RawDb raw(dbPath_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        raw.exec("CREATE TABLE devices(device_id TEXT PRIMARY KEY, display_name TEXT, protocol_type INTEGER, "
                 "is_active INTEGER, driverTypes TEXT);"
                 "INSERT INTO devices VALUES ('SN-1', 'Plug', 0, 1, '');");
    }
    const std::string before = readFile(dbPath_);
    std::ofstream(dbPath_ + ".bak-v0-20260101T000000Z") << "existing"; // 백업 이름 충돌로 실패시킨다

    DatabaseManager db;
    EXPECT_FALSE(db.init(dbPath_, "20260101T000000Z"));

    EXPECT_EQ(readFile(dbPath_), before);
    RawDb raw(dbPath_);
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name IN ('device_energy_stats','state_history_logs')"), "0");
}

// 전력·이력 테이블을 만들지 못하면 init은 실패한다(예전에는 결과를 무시하고 true)
TEST_F(DatabaseMigrationTest, TableCreationFailureMakesInitFail) {
    if (::geteuid() == 0) GTEST_SKIP() << "root는 읽기 전용 파일에도 쓸 수 있어 재현 불가";
    {
        RawDb raw(dbPath_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        raw.exec("CREATE TABLE devices(device_id TEXT PRIMARY KEY, display_name TEXT, protocol_type INTEGER, "
                 "is_active INTEGER, driverTypes TEXT, ip TEXT, deleted_at TEXT);"
                 "PRAGMA user_version = 1;");
    }
    ASSERT_EQ(::chmod(dbPath_.c_str(), 0444), 0);

    DatabaseManager db;
    const bool initialized = db.init(dbPath_);
    db.close();
    ::chmod(dbPath_.c_str(), 0644);

    EXPECT_FALSE(initialized);
}

}  // namespace
