#include <gtest/gtest.h>
#include <sqlite3.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "HTTPServer.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "httplib.h"
#include "json.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const std::string kToken = "energy-test-token-0123456789";

int findFreePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    const int port = ntohs(address.sin_port);
    close(fd);
    return port;
}

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
        server = std::make_unique<HTTPServer>(manager, kToken, port);
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
    DeviceManager manager{database, deviceDrivers, clock};
    FakeProtocolDriver matterFake;
    FakeProtocolDriver tcpFake;
    std::unique_ptr<HTTPServer> server;
    int port = 0;
};

// ---- REQ-03: 조회 실패는 통계에 반영하지 않는다 ----

TEST_F(EnergyPathTest, NormalThenFailureThenNormalAddsOnlyTheDelta) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {}, &matterFake);

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
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {}, &matterFake);
    matterFake.energyResult = Result<EnergyReading>::failure(ErrorCode::DeviceUnreachable, "누적 전력 값 없음");

    manager.runHealthCheck();

    EXPECT_EQ(statsRowCount("PLUG"), "0");
    EXPECT_TRUE(database.getEnergyHistory("PLUG").value->empty());
}

// D7: 32비트를 넘는 누적값(약 2.1 MWh 초과)도 잘리지 않는다
TEST_F(EnergyPathTest, LargeCumulativeValuesAreNotTruncated) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {}, &matterFake);
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
    manager.addDevice("ARD_10.0.0.9", "lamp", ProtocolType::TCP_DIY, {}, &tcpFake);
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{5, 50});
    startServer();

    auto missing = get("/api/devices/NOPE/energy");
    auto tcp = get("/api/devices/ARD_10.0.0.9/energy");

    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 400);
    EXPECT_EQ(json::parse(missing->body), json::parse(R"({"error":"등록되지 않은 기기입니다."})"));
    ASSERT_TRUE(tcp);
    EXPECT_EQ(tcp->status, 400);
    EXPECT_EQ(json::parse(tcp->body), json::parse(R"({"error":"서버 내부 데이터 처리 오류입니다."})"));
    EXPECT_EQ(statsRowCount("NOPE"), "0");
    EXPECT_EQ(statsRowCount("ARD_10.0.0.9"), "0");
    EXPECT_TRUE(matterFake.energyReadIds.empty()); // Matter 드라이버에 남의 ID를 묻지 않는다
}

TEST_F(EnergyPathTest, E6SuccessReturnsStateAndCorrectedTotal) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {}, &matterFake);
    manager.updateDeviceState("PLUG", "ON");
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{15230, 1048576});
    startServer();

    auto response = get("/api/devices/PLUG/energy");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body),
              json::parse(R"({"device_status":"ON","activePower_mW":15230,"totalEnergy_mWh":1048576})"));
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
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {}, &matterFake);
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{1, 100});
    RawDb raw(dbPath_);
    raw.exec("DROP TABLE device_energy_stats;");
    startServer();

    auto response = get("/api/devices/PLUG/energy");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"error":"서버 내부 데이터 처리 오류입니다."})"));
}

// ---- D5 ----

TEST_F(EnergyPathTest, InsertEnergyLogReturnsFalseWhenStoreFails) {
    RawDb raw(dbPath_);
    raw.exec("CREATE TRIGGER reject_log BEFORE INSERT ON state_history_logs BEGIN SELECT RAISE(ABORT, 'no'); END;");

    EXPECT_FALSE(database.insertEnergyLog("PLUG", 100));
}

// ---- G11 / H12: E7은 존재 확인 먼저, DB 오류는 200 + error ----

TEST_F(EnergyPathTest, E7ReportsDatabaseErrorInsteadOfEmptyList) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {}, &matterFake);
    RawDb raw(dbPath_);
    raw.exec("DROP TABLE state_history_logs;");
    startServer();

    auto response = get("/api/devices/PLUG/energy/history");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"error":"서버 내부 데이터 처리 오류입니다."})"));
}

TEST_F(EnergyPathTest, E7EmptyHistoryIsStillSuccess) {
    manager.addDevice("PLUG", "plug", ProtocolType::MATTER, {}, &matterFake);
    startServer();

    auto response = get("/api/devices/PLUG/energy/history");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"device_status":"UNKNOWN","logs":[]})"));
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

    const auto devices = database.loadAllDevices();

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
        EXPECT_TRUE(db.loadAllDevices().empty());
        EXPECT_FALSE(db.updateDeviceState("X", 1));
        EXPECT_FALSE(db.removeDevice("X"));
        EXPECT_FALSE(db.updateEnergyStat("X", 1, total));
        EXPECT_FALSE(db.insertEnergyLog("X", 1));
        EXPECT_FALSE(db.getEnergyHistory("X").ok());
        EXPECT_FALSE(db.initEnergyTable());
        EXPECT_FALSE(db.initHistoryTable());
    }
}

}  // namespace
