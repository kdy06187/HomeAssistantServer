#include <gtest/gtest.h>
#include <sqlite3.h>
#include "DatabaseManager.hpp"
#include "support/HistoryTestHelper.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "CommissioningService.hpp"
#include "HTTPServer.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "httplib.h"
#include "json.hpp"
#include "support/RawHttpClient.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

using json = nlohmann::json;
using testsupport::findFreePort;
namespace fs = std::filesystem;

namespace {

const std::string kToken = "energy-test-token-0123456789";

// DatabaseManager를 거치지 않고 같은 파일을 직접 보는 검증용 연결
class RawDb {
public:
    explicit RawDb(const std::string& path) { EXPECT_EQ(sqlite3_open(path.c_str(), &db_), SQLITE_OK); }
    ~RawDb() { sqlite3_close_v2(db_); }
    void exec(const std::string& sql) {
        char* err = nullptr;
        EXPECT_EQ(sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
        sqlite3_free(err);
    }
    std::string scalar(const std::string& sql) {
        sqlite3_stmt* stmt = nullptr;
        std::string out = "<none>";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            out = text ? reinterpret_cast<const char*>(text) : "<null>";
        }
        sqlite3_finalize(stmt);
        return out;
    }

private:
    sqlite3* db_ = nullptr;
};

// 임시 파일 DB + 가짜 드라이버. 운영 DB는 건드리지 않는다
class EnergyPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               (std::string("hac_energy_test_") + std::to_string(::getpid()) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        dbPath_ = (dir_ / "energy.db").string();
        ASSERT_TRUE(database.init(dbPath_));
        manager.registerDriver(ProtocolType::MATTER, &matterFake);
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
        // 실제 TCPDriver처럼 전력 조회를 지원하지 않는다
        tcpFake.energyResult = Result<EnergyReading>::failure(ErrorCode::UnsupportedOperation, "unsupported");
    }
    void TearDown() override {
        if (server) server->stop();
        database.close();
        fs::remove_all(dir_);
    }

    void startServer() {
        port = findFreePort();
        server = std::make_unique<HTTPServer>(manager, commissioning, kToken, port);
        ASSERT_TRUE(server->start());
        ASSERT_TRUE(server->waitUntilReady(std::chrono::seconds(3)));
    }
    httplib::Result get(const std::string& path) {
        httplib::Client client("127.0.0.1", port);
        return client.Get(path, httplib::Headers{{"Authorization", "Bearer " + kToken}});
    }
    std::string statsRowCount(const std::string& id) {
        RawDb raw(dbPath_);
        return raw.scalar("SELECT COUNT(*) FROM device_energy_stats WHERE device_id = '" + id + "'");
    }

    fs::path dir_;
    std::string dbPath_;
    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    CommissioningService commissioning{manager, {}, clock};
    FakeProtocolDriver matterFake;
    FakeProtocolDriver tcpFake;
    std::unique_ptr<HTTPServer> server;
    int port = 0;
};

// ---- REQ-03: 조회 실패는 통계에 반영하지 않는다 ----

TEST_F(EnergyPathTest, NormalThenFailureThenNormalAddsOnlyTheDelta) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});

    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{10, 100});
    ASSERT_TRUE(manager.readEnergy("PLUG").ok());
    matterFake.energyResult = Result<EnergyReading>::failure(ErrorCode::DeviceUnreachable, "누적 전력 값 없음");
    EXPECT_FALSE(manager.readEnergy("PLUG").ok());
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{10, 110});
    auto energy = manager.readEnergy("PLUG");

    ASSERT_TRUE(energy.ok());
    EXPECT_EQ(energy.value->totalEnergy_mWh, 110); // 실패를 0으로 저장했다면 100 + 110 = 210

    RawDb raw(dbPath_);
    EXPECT_EQ(raw.scalar("SELECT last_plug_mwh FROM device_energy_stats WHERE device_id='PLUG'"), "110");
}

TEST_F(EnergyPathTest, HealthCheckFailureDoesNotWriteHistoryOrStats) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    matterFake.energyResult = Result<EnergyReading>::failure(ErrorCode::DeviceUnreachable, "누적 전력 값 없음");

    manager.runHealthCheck();

    EXPECT_EQ(statsRowCount("PLUG"), "0");
    EXPECT_TRUE(latestLogs(database, "PLUG").value->empty());
}

// D7: 32비트를 넘는 누적값(약 2.1 MWh 초과)도 잘리지 않는다
TEST_F(EnergyPathTest, LargeCumulativeValuesAreNotTruncated) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    const int64_t big = 5'000'000'000LL;
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1, big});
    ASSERT_TRUE(manager.readEnergy("PLUG").ok());
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1, big + 7});

    auto energy = manager.readEnergy("PLUG");

    ASSERT_TRUE(energy.ok());
    EXPECT_EQ(energy.value->totalEnergy_mWh, big + 7);
}

// ---- REQ-04 / H6: 미등록·TCP 기기는 DB 행을 만들지 않는다 ----

TEST_F(EnergyPathTest, E6ForUnregisteredAndTcpDeviceCreatesNoStatsRow) {
    manager.addDevice("TCP_A1B2C3D4E5F6", "lamp", ProtocolType::TCP_DIY, {});
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{5, 50});
    startServer();

    auto missing = get("/api/devices/NOPE/energy");
    auto tcp = get("/api/devices/TCP_A1B2C3D4E5F6/energy");

    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
    EXPECT_EQ(json::parse(missing->body), json::parse(R"({"error":{"code":"DEVICE_NOT_FOUND","message":"등록되지 않은 기기입니다."}})"));
    ASSERT_TRUE(tcp);
    EXPECT_EQ(tcp->status, 400);
    EXPECT_EQ(json::parse(tcp->body),
              json::parse(R"({"error":{"code":"UNSUPPORTED_OPERATION","message":"이 기기에서 지원하지 않는 요청입니다."}})"));
    EXPECT_EQ(statsRowCount("NOPE"), "0");
    EXPECT_EQ(statsRowCount("TCP_A1B2C3D4E5F6"), "0");
    EXPECT_TRUE(matterFake.energyReadIds.empty()); // Matter 드라이버에 남의 ID를 묻지 않는다
}

TEST_F(EnergyPathTest, E6SuccessReturnsStateAndCorrectedTotal) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    manager.updateDeviceState("PLUG", "ON");
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{15230, 1048576});
    startServer();

    auto response = get("/api/devices/PLUG/energy");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body),
              json::parse(R"({"deviceId":"PLUG","state":"ON","activePower_mW":15230,"totalEnergy_mWh":1048576})"));
}

// ---- 6b: 같은 기기 전력 조회는 조회~저장이 직렬화된다 ----

// 먼저 시작한 조회(100)가 늦게 끝나고 나중 조회(120)가 먼저 끝나면, 락이 없을 때는
// 120 저장 뒤 100이 "카운터 초기화"로 보여 합계가 220이 된다. 정답은 120.
TEST_F(EnergyPathTest, ConcurrentReadsOfSameDeviceDoNotDoubleCount) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    std::promise<void> firstEntered;
    std::promise<void> releaseFirst;
    std::shared_future<void> releaseSignal = releaseFirst.get_future().share();
    std::atomic<int> calls{0};
    matterFake.onReadEnergy = [&](const std::string&) {
        if (calls.fetch_add(1) == 0) {
            firstEntered.set_value();
            releaseSignal.wait_for(std::chrono::seconds(5));
            return Result<EnergyReading>::success(EnergyReading{1, 100});
        }
        return Result<EnergyReading>::success(EnergyReading{1, 120});
    };

    auto first = std::async(std::launch::async, [this] { return manager.readEnergy("PLUG"); });
    ASSERT_EQ(firstEntered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto second = std::async(std::launch::async, [this] { return manager.readEnergy("PLUG"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // 락이 없다면 두 번째가 여기서 먼저 끝난다
    releaseFirst.set_value();

    ASSERT_TRUE(first.get().ok());
    auto later = second.get();
    ASSERT_TRUE(later.ok());
    EXPECT_EQ(later.value->totalEnergy_mWh, 120);
    RawDb raw(dbPath_);
    EXPECT_EQ(raw.scalar("SELECT real_total_mwh FROM device_energy_stats WHERE device_id='PLUG'"), "120");
}

// 조회 도중 기기가 삭제되면 통계 행을 만들지 않는다
TEST_F(EnergyPathTest, DeviceRemovedDuringReadCreatesNoStatsRow) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> releaseSignal = release.get_future().share();
    matterFake.onReadEnergy = [&](const std::string&) {
        entered.set_value();
        releaseSignal.wait_for(std::chrono::seconds(5));
        return Result<EnergyReading>::success(EnergyReading{1, 100});
    };

    auto reading = std::async(std::launch::async, [this] { return manager.readEnergy("PLUG"); });
    ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    manager.removeDevice("PLUG"); // 전력 락을 기다리지 않고 바로 끝나야 한다
    release.set_value();

    const auto result = reading.get();
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error.code, ErrorCode::DeviceNotFound);
    EXPECT_EQ(statsRowCount("PLUG"), "0");
}

// 느린 전력 조회 중에도 목록·다른 기기 제어는 1초 안에 끝난다
TEST_F(EnergyPathTest, SlowEnergyReadDoesNotBlockOtherRequests) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    manager.addDevice("LAMP", "lamp", ProtocolType::TCP_DIY, {});
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> releaseSignal = release.get_future().share();
    matterFake.onReadEnergy = [&](const std::string&) {
        entered.set_value();
        releaseSignal.wait_for(std::chrono::seconds(5));
        return Result<EnergyReading>::success(EnergyReading{1, 100});
    };

    auto reading = std::async(std::launch::async, [this] { return manager.readEnergy("PLUG"); });
    ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto others = std::async(std::launch::async, [this] {
        return manager.getAllDevices().size() == 2 &&
               manager.executeCommand("LAMP", "ON").outcome == CommandOutcome::Succeeded;
    });
    const auto status = others.wait_for(std::chrono::seconds(1));
    release.set_value();

    EXPECT_EQ(status, std::future_status::ready) << "전력 조회 중 다른 요청이 1초 넘게 막혔다";
    EXPECT_TRUE(others.get());
    EXPECT_TRUE(reading.get().ok());
}

// 드라이버의 "매핑 없음"은 등록된 기기에 대해 내부 오류로 답한다(E6에 "등록되지 않은 기기"로 보이지 않게)
TEST_F(EnergyPathTest, DriverDeviceNotFoundIsReportedAsInternal) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    matterFake.energyResult = Result<EnergyReading>::failure(ErrorCode::DeviceNotFound, "시리얼 매핑 없음");

    const auto energy = manager.readEnergy("PLUG");

    ASSERT_FALSE(energy.ok());
    EXPECT_EQ(energy.error.code, ErrorCode::Internal);
}

// 드라이버가 돌려준 코드는 DeviceNotFound를 뺀 나머지를 그대로 전달한다(Cancelled 등이 왜곡되지 않게)
TEST_F(EnergyPathTest, DriverFailureCodesArePassedThroughUnchanged) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    for (ErrorCode code : {ErrorCode::DeviceUnreachable, ErrorCode::Cancelled, ErrorCode::UnsupportedOperation,
                           ErrorCode::Internal}) {
        matterFake.energyResult = Result<EnergyReading>::failure(code, "driver failure");
        const auto energy = manager.readEnergy("PLUG");
        ASSERT_FALSE(energy.ok());
        EXPECT_EQ(energy.error.code, code);
    }
}

// 상태를 확인하지 못한 기기(PAIRING·ERROR 포함)는 헬스체크에서 전력을 조회하지 않는다
TEST_F(EnergyPathTest, HealthCheckSkipsEnergyForUnconfirmedStates) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1, 100});
    for (const char* state : {"PAIRING", "ERROR", "OFFLINE", "UNKNOWN"}) {
        matterFake.stateToReturn = state;
        manager.runHealthCheck();
    }
    EXPECT_TRUE(matterFake.energyReadIds.empty());

    matterFake.stateToReturn = "ON";
    manager.runHealthCheck();
    EXPECT_EQ(matterFake.energyReadIds.size(), 1u);
}

// E6: 음수 누적값은 저장하지 않는다
TEST_F(EnergyPathTest, NegativeCumulativeIsRejected) {
    int64_t total = 0;
    ASSERT_TRUE(database.updateEnergyStat("PLUG", 100, total));
    EXPECT_FALSE(database.updateEnergyStat("PLUG", -1, total));
    RawDb raw(dbPath_);
    EXPECT_EQ(raw.scalar("SELECT real_total_mwh || '/' || last_plug_mwh FROM device_energy_stats"), "100/100");
}

// E8: 같은 초에 쌓인 기록도 넣은 순서대로, limit<=0은 거부
TEST_F(EnergyPathTest, HistoryOrdersSameSecondByInsertionAndRejectsNonPositiveLimit) {
    RawDb raw(dbPath_);
    raw.exec("INSERT INTO state_history_logs (device_id, total_mwh, timestamp) VALUES "
             "('PLUG', 10, '2026-11-19 00:00:00'), ('PLUG', 20, '2026-11-19 00:00:00'), ('PLUG', 30, '2026-11-19 00:00:00');");

    auto latestTwo = latestLogs(database, "PLUG", 2);
    ASSERT_TRUE(latestTwo.ok());
    ASSERT_EQ(latestTwo.value->size(), 2u);
    EXPECT_EQ((*latestTwo.value)[0].total_mwh, 20);
    EXPECT_EQ((*latestTwo.value)[1].total_mwh, 30);

    EXPECT_EQ(latestLogs(database, "PLUG", 0).error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(latestLogs(database, "PLUG", -1).error.code, ErrorCode::InvalidRequest);
}

// E14: E7 응답 필드·순서 전체
TEST_F(EnergyPathTest, E7ReturnsExactFieldsOldestFirst) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    manager.updateDeviceState("PLUG", "ON");
    RawDb raw(dbPath_);
    raw.exec("INSERT INTO state_history_logs (device_id, total_mwh, timestamp) VALUES "
             "('PLUG', 1048576, '2026-11-16 09:30:00'), ('PLUG', 1040000, '2026-11-16 09:20:00'), ('OTHER', 5, '2026-11-16 09:25:00');");
    startServer();

    auto response = get("/api/devices/PLUG/energy/history");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"deviceId":"PLUG","state":"ON","logs":[
        {"timestamp":"2026-11-16T09:20:00Z","totalEnergy_mWh":1040000},
        {"timestamp":"2026-11-16T09:30:00Z","totalEnergy_mWh":1048576}],"truncated":false})"));
}

TEST_F(EnergyPathTest, E7UnregisteredDeviceReturns404) {
    startServer();
    auto response = get("/api/devices/NOPE/energy/history");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"error":{"code":"DEVICE_NOT_FOUND","message":"등록되지 않은 기기입니다."}})"));
}

TEST_F(EnergyPathTest, E6DriverFailureReturns502) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    matterFake.energyResult = Result<EnergyReading>::failure(ErrorCode::DeviceUnreachable, "누적 전력 값 없음");
    startServer();
    auto response = get("/api/devices/PLUG/energy");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 502);
    EXPECT_EQ(json::parse(response->body),
              json::parse(R"({"error":{"code":"DEVICE_UNREACHABLE","message":"기기가 응답하지 않습니다."}})"));
    EXPECT_EQ(statsRowCount("PLUG"), "0");
}

// ---- D4: 통계 조회가 실패하면 "최초 등록"으로 보지 않는다 ----

TEST_F(EnergyPathTest, StatSelectFailureRollsBackAndReturnsFalse) {
    int64_t total = 0;
    ASSERT_TRUE(database.updateEnergyStat("PLUG", 100, total));
    RawDb raw(dbPath_);
    raw.exec("ALTER TABLE device_energy_stats RENAME COLUMN last_plug_mwh TO broken;"); // SELECT 준비가 실패하게 만든다

    EXPECT_FALSE(database.updateEnergyStat("PLUG", 150, total));

    EXPECT_EQ(raw.scalar("SELECT real_total_mwh FROM device_energy_stats WHERE device_id='PLUG'"), "100");
    // 롤백이 끝나 다음 트랜잭션을 시작할 수 있어야 한다
    raw.exec("ALTER TABLE device_energy_stats RENAME COLUMN broken TO last_plug_mwh;");
    ASSERT_TRUE(database.updateEnergyStat("PLUG", 150, total));
    EXPECT_EQ(total, 150);
}

TEST_F(EnergyPathTest, E6ReturnsInternalErrorWhenStatsCannotBeSaved) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1, 100});
    RawDb raw(dbPath_);
    raw.exec("DROP TABLE device_energy_stats;");
    startServer();

    auto response = get("/api/devices/PLUG/energy");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"error":{"code":"INTERNAL_ERROR","message":"서버 내부 오류입니다."}})"));
}

// ---- D5 ----

TEST_F(EnergyPathTest, InsertEnergyLogReturnsFalseWhenStoreFails) {
    RawDb raw(dbPath_);
    raw.exec("CREATE TRIGGER reject_log BEFORE INSERT ON state_history_logs BEGIN SELECT RAISE(ABORT, 'no'); END;");

    EXPECT_FALSE(database.insertEnergyLog("PLUG", 100, "2026-11-19 00:00:00"));
}

// ---- G11 / H12: E7은 존재 확인 먼저, DB 오류는 500 ----

TEST_F(EnergyPathTest, E7ReportsDatabaseErrorInsteadOfEmptyList) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    RawDb raw(dbPath_);
    raw.exec("DROP TABLE state_history_logs;");
    startServer();

    auto response = get("/api/devices/PLUG/energy/history");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"error":{"code":"INTERNAL_ERROR","message":"서버 내부 오류입니다."}})"));
}

TEST_F(EnergyPathTest, E7EmptyHistoryIsStillSuccess) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    startServer();

    auto response = get("/api/devices/PLUG/energy/history");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"deviceId":"PLUG","state":"UNKNOWN","logs":[],"truncated":false})"));
}

// ---- D2: 재등록은 행을 지우지 않고 갱신한다 ----

TEST_F(EnergyPathTest, ReinsertKeepsIpAndClearsDeletedAt) {
    Device device("ARD_10.0.0.5", "lamp", ProtocolType::TCP_DIY);
    device.state = "OFF";
    ASSERT_TRUE(database.insertDevice(device));
    RawDb raw(dbPath_);
    raw.exec("UPDATE devices SET ip='10.0.0.5', deleted_at='2026-11-19 00:00:00' WHERE device_id='ARD_10.0.0.5';");

    device.name = "desk lamp";
    device.state = "ON";
    ASSERT_TRUE(database.insertDevice(device));

    EXPECT_EQ(raw.scalar("SELECT ip FROM devices WHERE device_id='ARD_10.0.0.5'"), "10.0.0.5");
    EXPECT_EQ(raw.scalar("SELECT deleted_at FROM devices WHERE device_id='ARD_10.0.0.5'"), "<null>");
    EXPECT_EQ(raw.scalar("SELECT display_name || '/' || is_active FROM devices"), "desk lamp/1");
    EXPECT_EQ(raw.scalar("SELECT COUNT(*) FROM devices"), "1");
}

// ---- D3: NULL 열이 있어도 로드가 죽지 않는다 ----

TEST_F(EnergyPathTest, LoadSkipsNullIdAndAcceptsNullName) {
    RawDb raw(dbPath_);
    raw.exec("INSERT INTO devices (device_id, display_name, protocol_type, is_active) VALUES ('SN-1', NULL, 0, 1);"
             "INSERT INTO devices (device_id, display_name, protocol_type, is_active) VALUES (NULL, 'ghost', 0, 1);");

    const auto devices = database.loadActiveDevices();

    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].id, "SN-1");
    EXPECT_EQ(devices[0].name, "");
}

// ---- D6: 열리지 않았거나 닫힌 DB에도 죽지 않는다 ----

TEST(EnergyPathClosedDbTest, CallsOnUnopenedOrClosedDatabaseFailSafely) {
    for (const bool openThenClose : {false, true}) {
        DatabaseManager db;
        if (openThenClose) {
            ASSERT_TRUE(db.init(":memory:"));
            db.close();
        }
        int64_t total = 0;
        Device device("X", "x", ProtocolType::MATTER);
        EXPECT_FALSE(db.insertDevice(device));
        EXPECT_TRUE(db.loadActiveDevices().empty());
        EXPECT_FALSE(db.updateDeviceState("X", 1));
        EXPECT_FALSE(db.markDeleted("X", "2026-11-19 00:00:00").ok());
        EXPECT_FALSE(db.updateEnergyStat("X", 1, total));
        EXPECT_FALSE(db.insertEnergyLog("X", 1, "2026-11-19 00:00:00"));
        EXPECT_FALSE(latestLogs(db, "X").ok());
        EXPECT_FALSE(db.initEnergyTable());
        EXPECT_FALSE(db.initHistoryTable());
    }
}

}  // namespace

// 숨김 저장이 실패하면 500이고 기기는 목록에 그대로 있다
TEST_F(EnergyPathTest, E4HideFailureReturns500AndKeepsDevice) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {});
    RawDb raw(dbPath_);
    raw.exec("CREATE TRIGGER block_hide BEFORE UPDATE OF deleted_at ON devices BEGIN SELECT RAISE(ABORT, 'blocked'); END;");
    startServer();

    httplib::Client client("127.0.0.1", port);
    auto response = client.Post("/api/devices/delete", httplib::Headers{{"Authorization", "Bearer " + kToken}},
                                R"({"deviceId":"PLUG"})", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"error":{"code":"INTERNAL_ERROR","message":"서버 내부 오류입니다."}})"));
    EXPECT_EQ(manager.getAllDevices().size(), 1u);
}
