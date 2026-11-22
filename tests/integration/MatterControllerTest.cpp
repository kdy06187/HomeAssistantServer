#include <gtest/gtest.h>
#include <sqlite3.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "MatterController.hpp"
#include "fakes/FakeChipToolRunner.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

const std::string kChipTool = "/fake/chip-tool";
const std::string kGoodCode = "34970112332";
constexpr uint32_t kGoodPin = 20202021;

Result<uint32_t> fakePinParser(const std::string& code) {
    if (code == kGoodCode) return Result<uint32_t>::success(kGoodPin);
    return Result<uint32_t>::failure(ErrorCode::CommissionFailed, "bad code");
}

bool startsWith(const std::vector<std::string>& sub, std::initializer_list<const char*> prefix) {
    std::size_t i = 0;
    for (const char* part : prefix) {
        if (i >= sub.size() || sub[i] != part) return false;
        ++i;
    }
    return true;
}

// 제한 시간 안에 끝나지 않으면 프로세스를 끝낸다(교착이면 영원히 멈추는 대신 실패)
class DeadlockWatchdog {
public:
    explicit DeadlockWatchdog(std::chrono::seconds limit)
        : thread_([this, limit] {
              std::unique_lock<std::mutex> lock(mutex_);
              if (!cv_.wait_for(lock, limit, [this] { return done_; })) {
                  std::cerr << "[MatterControllerTest] 제한 시간 초과: 교착 의심" << std::endl;
                  std::abort();
              }
          }) {}
    ~DeadlockWatchdog() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};

// 가짜 러너 + 임시 등록 파일로 MatterController를 조립한다(실제 chip-tool·Matter SDK 없음)
class MatterControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               (std::string("hac_matter_test_") + std::to_string(::getpid()) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        configPath_ = (dir_ / "registered_devices.txt").string();
        std::ofstream(configPath_) << "SN-1,101\nSN-2,102\n";
        storagePath_ = (dir_ / "chip-tool").string();
        controller = std::make_unique<MatterController>(runner, fakePinParser, kChipTool, configPath_, storagePath_);
        ASSERT_TRUE(controller->Initialize());
    }
    void TearDown() override {
        EXPECT_TRUE(runner.waitUntilIdle());
        controller.reset();
        fs::remove_all(dir_);
    }

    std::string readConfig() {
        std::ifstream in(configPath_);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    std::vector<std::vector<std::string>> subcommands() {
        std::vector<std::vector<std::string>> result;
        for (const auto& call : runner.callsSnapshot()) result.emplace_back(call.args.begin() + 1, call.args.end());
        return result;
    }

    fs::path dir_;
    std::string configPath_;
    std::string storagePath_;
    std::vector<std::string> storageArgs() const { return {"--storage-directory", storagePath_}; }
    FakeChipToolRunner runner;
    std::unique_ptr<MatterController> controller;
};

// ---- 인자·제한 시간 ----

TEST_F(MatterControllerTest, ControlPassesArgumentArrayWithConfiguredPathAndTimeout) {
    EXPECT_TRUE(controller->sendCommand("SN-1", "ON"));
    const auto calls = runner.callsSnapshot();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].args, (std::vector<std::string>{kChipTool, "onoff", "on", "101", "1", "--storage-directory", storagePath_}));
    EXPECT_EQ(calls[0].timeout, MatterController::kCommandTimeout);
    EXPECT_TRUE(calls[0].stdinText.empty());
}

TEST_F(MatterControllerTest, ControlFailureMarksOfflineAndAutomaticReadIsSkipped) {
    runner.respond = [](const std::vector<std::string>&) { return FakeChipToolOutput::timeout(); };
    EXPECT_FALSE(controller->sendCommand("SN-1", "OFF"));

    EXPECT_EQ(controller->readDeviceState("SN-1", false), "OFFLINE");  // 오프라인 명부 → chip-tool 생략
    EXPECT_EQ(runner.callsSnapshot().size(), 1u);

    runner.respond = [](const std::vector<std::string>&) { return FakeChipToolOutput::ok("Data = true"); };
    EXPECT_EQ(controller->readDeviceState("SN-1", true), "ON");  // 수동 새로고침은 실행, 온라인 복구
    EXPECT_EQ(controller->readDeviceState("SN-1", false), "ON");
}

TEST_F(MatterControllerTest, ReadStateParsesOnOff) {
    runner.respond = [](const std::vector<std::string>& sub) {
        EXPECT_TRUE(startsWith(sub, {"onoff", "read", "on-off", "102", "1"}));
        return FakeChipToolOutput::ok("Data = false");
    };
    EXPECT_EQ(controller->readDeviceState("SN-2", true), "OFF");
}

TEST_F(MatterControllerTest, ReadEnergyRunsBothReadsAndParses) {
    runner.respond = [](const std::vector<std::string>& sub) {
        if (startsWith(sub, {"electricalpowermeasurement"})) return FakeChipToolOutput::ok("ActivePower: 15230");
        return FakeChipToolOutput::ok("Energy: 1048576");
    };
    const auto reading = controller->readEnergy("SN-1");
    ASSERT_TRUE(reading.ok());
    EXPECT_EQ(reading.value->activePower_mW, 15230);
    EXPECT_EQ(reading.value->cumulative_mWh, 1048576);
    EXPECT_EQ(runner.callsSnapshot().size(), 2u);
}

TEST_F(MatterControllerTest, ReadEnergySkipsSecondCallWhenFirstIsOffline) {
    runner.respond = [](const std::vector<std::string>&) { return FakeChipToolOutput::timeout(); };
    const auto reading = controller->readEnergy("SN-1");
    EXPECT_EQ(reading.error.code, ErrorCode::DeviceUnreachable);
    EXPECT_EQ(runner.callsSnapshot().size(), 1u);
}

TEST_F(MatterControllerTest, ReadEnergyForUnknownSerialRunsNothing) {
    EXPECT_EQ(controller->readEnergy("NOPE").error.code, ErrorCode::DeviceNotFound);
    EXPECT_TRUE(runner.callsSnapshot().empty());
}

// ---- chip-tool 저장소 ----

// 모든 chip-tool 실행(제어·상태·전력 2종·페어링·시리얼·해제)이 같은 저장소 옵션으로 끝난다
TEST_F(MatterControllerTest, EveryChipToolCallUsesStorageDirectory) {
    runner.respond = [](const std::vector<std::string>& sub) {
        if (startsWith(sub, {"pairing", "ble-wifi"})) return FakeChipToolOutput::ok("Device commissioning completed with success");
        if (startsWith(sub, {"basicinformation"})) return FakeChipToolOutput::ok("SerialNumber: SN-NEW");
        if (startsWith(sub, {"onoff", "read"})) return FakeChipToolOutput::ok("Data = true");
        if (startsWith(sub, {"electricalpowermeasurement"})) return FakeChipToolOutput::ok("ActivePower: 1500");
        if (startsWith(sub, {"electricalenergymeasurement"})) return FakeChipToolOutput::ok("Energy: 2000");
        return FakeChipToolOutput::ok("");
    };
    EXPECT_TRUE(controller->sendCommand("SN-1", "ON"));
    EXPECT_TRUE(controller->sendCommand("SN-1", "OFF"));
    controller->readDeviceState("SN-1", true);
    controller->readEnergy("SN-1");
    ASSERT_TRUE(controller->commission(CommissionRequest{"plug", kGoodCode, "wifi", "pw"}, {}).ok());
    EXPECT_TRUE(controller->unpairDevice("SN-2"));

    std::set<std::string> kinds;
    for (const auto& call : runner.callsSnapshot()) {
        ASSERT_GE(call.args.size(), 4u);
        EXPECT_EQ(std::vector<std::string>(call.args.end() - 2, call.args.end()), storageArgs()) << call.args[1];
        kinds.insert(call.args[1] + " " + call.args[2]);
    }
    EXPECT_EQ(kinds, (std::set<std::string>{"onoff on", "onoff off", "onoff read", "electricalpowermeasurement read",
                                            "electricalenergymeasurement read", "pairing ble-wifi",
                                            "basicinformation read", "pairing unpair"}));
}

// 저장소 폴더는 Initialize가 0700으로 만들고, 이미 있으면 권한을 0700으로 맞춘다
TEST_F(MatterControllerTest, InitializeCreatesPrivateStorageDirectory) {
    ASSERT_TRUE(fs::is_directory(storagePath_));
    EXPECT_EQ(fs::status(storagePath_).permissions() & fs::perms::all, fs::perms::owner_all);

    fs::permissions(storagePath_, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);
    MatterController again(runner, fakePinParser, kChipTool, configPath_, storagePath_);
    ASSERT_TRUE(again.Initialize());
    EXPECT_EQ(fs::status(storagePath_).permissions() & fs::perms::all, fs::perms::owner_all);
}

// 저장소 자리에 파일이 있으면 초기화 실패(chip-tool이 엉뚱한 곳을 쓰지 않게)
TEST_F(MatterControllerTest, InitializeFailsWhenStoragePathIsAFile) {
    const std::string filePath = (dir_ / "not-a-dir").string();
    std::ofstream(filePath) << "x";
    MatterController broken(runner, fakePinParser, kChipTool, configPath_, filePath);
    EXPECT_FALSE(broken.Initialize());
}

// ---- 커미셔닝 ----

TEST_F(MatterControllerTest, CommissionPairsReadsSerialAndSavesMapping) {
    runner.respond = [](const std::vector<std::string>& sub) {
        if (startsWith(sub, {"pairing", "ble-wifi"})) return FakeChipToolOutput::ok("Device commissioning completed with success");
        if (startsWith(sub, {"basicinformation"})) return FakeChipToolOutput::ok("SerialNumber: SN-NEW");
        return FakeChipToolOutput::ok("");
    };
    const auto outcome = controller->commission(CommissionRequest{"plug", kGoodCode, "home wifi", "p@ss w0rd"}, {});
    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.value->deviceId, "SN-NEW");

    const auto calls = runner.callsSnapshot();
    ASSERT_GE(calls.size(), 2u);
    const auto& pairing = calls[0];
    const std::string nodeId = pairing.args[3];
    // 셸 없이 인자 하나씩: 공백이 든 SSID·비밀번호도 한 인자
    EXPECT_EQ(pairing.args, (std::vector<std::string>{kChipTool, "pairing", "ble-wifi", nodeId, "str:home wifi", "str:p@ss w0rd",
                                                      std::to_string(kGoodPin), "3830", "--paa-trust-store-path",
                                                      "../paa_certs/paa-root-certs", "--storage-directory", storagePath_}));
    EXPECT_EQ(pairing.timeout, MatterController::kPairingTimeout);
    EXPECT_NE(std::find(pairing.secretsToMask.begin(), pairing.secretsToMask.end(), "p@ss w0rd"), pairing.secretsToMask.end());
    EXPECT_NE(std::find(pairing.secretsToMask.begin(), pairing.secretsToMask.end(), kGoodCode), pairing.secretsToMask.end());
    EXPECT_NE(std::find(pairing.secretsToMask.begin(), pairing.secretsToMask.end(), std::to_string(kGoodPin)), pairing.secretsToMask.end());
    EXPECT_EQ(calls[1].timeout, MatterController::kSlowCommandTimeout);  // 시리얼 읽기

    EXPECT_NE(readConfig().find("SN-NEW," + nodeId + "\n"), std::string::npos);
    EXPECT_FALSE(controller->isPairing(std::stoull(nodeId)));
    EXPECT_TRUE(controller->sendCommand("SN-NEW", "ON"));
    EXPECT_EQ(runner.callsSnapshot().back().args[3], nodeId);
}

TEST_F(MatterControllerTest, BadSetupCodeFailsWithoutRunningChipTool) {
    const auto outcome = controller->commission(CommissionRequest{"plug", "000", "ssid", "pw"}, {});
    EXPECT_EQ(outcome.error.code, ErrorCode::CommissionFailed);
    EXPECT_TRUE(runner.callsSnapshot().empty());
}

// G7: 페어링 도중 예외가 나도 "페어링 중" 표시가 풀린다
TEST_F(MatterControllerTest, PairingFlagIsClearedEvenWhenRunnerThrows) {
    std::atomic<uint64_t> seenNodeId{0};
    runner.respond = [&](const std::vector<std::string>& sub) -> ProcessResult {
        seenNodeId = std::stoull(sub[2]);
        EXPECT_TRUE(controller->isPairing(seenNodeId));  // 실행 중에는 페어링 중(러너는 락 밖에서 불린다)
        throw std::runtime_error("runner failure");
    };
    EXPECT_THROW(controller->commission(CommissionRequest{"plug", kGoodCode, "ssid", "pw"}, {}), std::runtime_error);
    ASSERT_NE(seenNodeId.load(), 0u);
    EXPECT_FALSE(controller->isPairing(seenNodeId));
}

// G7: 같은 순간 여러 커미셔닝도 서로 다른 노드 ID를 받고, 이미 쓰는 번호는 피한다
TEST_F(MatterControllerTest, ConcurrentCommissionsGetDistinctNodeIds) {
    DeadlockWatchdog watchdog(std::chrono::seconds(30));
    std::mutex seenMutex;
    std::vector<std::string> pairingNodeIds;
    runner.respond = [&](const std::vector<std::string>& sub) {
        if (startsWith(sub, {"pairing", "ble-wifi"})) {
            {
                std::lock_guard<std::mutex> lock(seenMutex);
                pairingNodeIds.push_back(sub[2]);
            }
            std::this_thread::sleep_for(20ms);
            return FakeChipToolOutput::ok("Device commissioning completed with success");
        }
        if (startsWith(sub, {"basicinformation"})) return FakeChipToolOutput::ok("SerialNumber: SN-" + sub[3]);
        return FakeChipToolOutput::ok("");
    };
    std::vector<std::thread> workers;
    for (int i = 0; i < 6; ++i) {
        workers.emplace_back([&] {
            EXPECT_TRUE(controller->commission(CommissionRequest{"plug", kGoodCode, "ssid", "pw"}, {}).ok());
        });
    }
    for (auto& worker : workers) worker.join();

    std::set<std::string> unique(pairingNodeIds.begin(), pairingNodeIds.end());
    EXPECT_EQ(unique.size(), pairingNodeIds.size());
    EXPECT_EQ(unique.count("101"), 0u);
    EXPECT_EQ(unique.count("102"), 0u);
}

// ---- 해제 ----

TEST_F(MatterControllerTest, UnpairRemovesMappingAndFileLineThenRunsUnpairWithSlowTimeout) {
    EXPECT_TRUE(controller->unpairDevice("SN-1"));

    // 반환 시점에 chip-tool 호출이 이미 끝나 있다(기다릴 필요 없음)
    EXPECT_EQ(readConfig(), "SN-2,102\n");
    const auto calls = runner.callsSnapshot();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].args, (std::vector<std::string>{kChipTool, "pairing", "unpair", "101", "--storage-directory", storagePath_}));
    EXPECT_EQ(calls[0].timeout, MatterController::kSlowCommandTimeout);
    EXPECT_EQ(controller->readEnergy("SN-1").error.code, ErrorCode::DeviceNotFound);  // 매핑도 사라짐
}

// 오프라인 기기: chip-tool이 시간 초과여도 등록 정보는 정리되고 true
TEST_F(MatterControllerTest, UnpairOfOfflineDeviceStillReturnsTrueAndCleansUp) {
    runner.respond = [](const std::vector<std::string>&) { return FakeChipToolOutput::timeout(); };

    EXPECT_TRUE(controller->unpairDevice("SN-1"));

    EXPECT_EQ(runner.callsSnapshot().size(), 1u);
    EXPECT_EQ(readConfig(), "SN-2,102\n");
}

// 종료 중(cancelAll 뒤)에는 러너가 바로 취소 결과를 돌려주므로 unpair도 즉시 끝난다
TEST_F(MatterControllerTest, UnpairAfterCancelAllReturnsImmediately) {
    runner.respond = [this](const std::vector<std::string>&) {
        return runner.cancelled ? ProcessResult{-1, "", false, true, false} : FakeChipToolOutput::ok("");
    };
    runner.cancelAll();

    const auto begin = std::chrono::steady_clock::now();
    EXPECT_TRUE(controller->unpairDevice("SN-2"));

    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(1));
    EXPECT_EQ(readConfig(), "SN-1,101\n");
}

// G8: 매핑이 없는 시리얼은 0번 노드에 unpair를 보내지 않는다
TEST_F(MatterControllerTest, UnpairOfUnknownSerialRunsNothing) {
    EXPECT_TRUE(controller->unpairDevice("NOPE"));
    EXPECT_TRUE(runner.callsSnapshot().empty());
    EXPECT_EQ(readConfig(), "SN-1,101\nSN-2,102\n");
}

// ---- 락(G12, TSan 빌드에서 의미가 크다) ----

// 러너는 MatterController의 락을 쥐지 않은 상태에서 불린다: 러너 안에서 다시 상태를 물어도 멈추지 않는다
TEST_F(MatterControllerTest, RunnerIsCalledWithoutHoldingControllerLocks) {
    DeadlockWatchdog watchdog(std::chrono::seconds(10));
    runner.respond = [&](const std::vector<std::string>& sub) {
        if (startsWith(sub, {"onoff", "on"})) {
            controller->isPairing(101);                          // stateMutex_
            controller->readEnergy("NOPE");                      // stateMutex_(매핑 조회)
        }
        return FakeChipToolOutput::ok("Data = true");
    };
    EXPECT_TRUE(controller->sendCommand("SN-1", "ON"));
}

TEST_F(MatterControllerTest, ConcurrentControlReadEnergyCommissionAndUnpairAreRaceFree) {
    DeadlockWatchdog watchdog(std::chrono::seconds(60));
    runner.respond = [](const std::vector<std::string>& sub) {
        if (startsWith(sub, {"pairing", "ble-wifi"})) return FakeChipToolOutput::ok("Device commissioning completed with success");
        if (startsWith(sub, {"basicinformation"})) return FakeChipToolOutput::ok("SerialNumber: SN-" + sub[3]);
        if (startsWith(sub, {"electricalpowermeasurement"})) return FakeChipToolOutput::ok("ActivePower: 10");
        if (startsWith(sub, {"electricalenergymeasurement"})) return FakeChipToolOutput::ok("Energy: 20");
        if (startsWith(sub, {"onoff", "read"})) return FakeChipToolOutput::ok("Data = true");
        return FakeChipToolOutput::ok("");
    };
    constexpr int kIterations = 40;
    std::atomic<bool> start{false};
    auto waitStart = [&] { while (!start) std::this_thread::yield(); };
    std::vector<std::thread> workers;
    workers.emplace_back([&] { waitStart(); for (int i = 0; i < kIterations; ++i) controller->sendCommand(i % 2 ? "SN-1" : "SN-2", i % 2 ? "ON" : "OFF"); });
    workers.emplace_back([&] { waitStart(); for (int i = 0; i < kIterations; ++i) controller->readDeviceState("SN-2", i % 2 == 0); });
    workers.emplace_back([&] { waitStart(); for (int i = 0; i < kIterations; ++i) controller->readEnergy("SN-1"); });
    workers.emplace_back([&] {
        waitStart();
        for (int i = 0; i < kIterations / 4; ++i) {
            auto outcome = controller->commission(CommissionRequest{"plug", kGoodCode, "ssid", "pw"}, {});
            if (outcome.ok()) {
                controller->unpairDevice(outcome.value->deviceId);
            }
        }
    });
    start = true;
    for (auto& worker : workers) worker.join();
    ASSERT_TRUE(runner.waitUntilIdle());

    // 처음 두 기기는 그대로, 새로 등록·해제한 기기는 파일에 남지 않는다
    EXPECT_EQ(readConfig(), "SN-1,101\nSN-2,102\n");
}

// ---- 8c: 실행 결과(ProcessResult)를 판정에 쓴다 ----

// 시간 초과·비정상 종료면 출력에 성공 표지가 있어도 실패이고 오프라인으로 본다
TEST_F(MatterControllerTest, TimeoutOrAbnormalExitIsFailureEvenWithSuccessMarker) {
    runner.respond = [](const std::vector<std::string>&) { return ProcessResult{128 + 15, "CHIP_NO_ERROR", true, false, false}; };
    EXPECT_FALSE(controller->sendCommand("SN-1", "ON"));
    EXPECT_EQ(controller->readDeviceState("SN-1", false), "OFFLINE");  // 오프라인 명부에 들어감(실행 생략)
    EXPECT_EQ(runner.callsSnapshot().size(), 1u);

    runner.respond = [](const std::vector<std::string>&) { return ProcessResult{1, "CHIP_NO_ERROR\nData = true", false, false, false}; };
    EXPECT_EQ(controller->readDeviceState("SN-2", true), "OFFLINE");
}

// 취소(서버 종료 등)는 실패지만 기기를 오프라인으로 표시하지 않는다
TEST_F(MatterControllerTest, CancelledRunFailsWithoutMarkingOffline) {
    runner.respond = [](const std::vector<std::string>&) { return ProcessResult{-1, "", false, true, false}; };
    EXPECT_FALSE(controller->sendCommand("SN-1", "ON"));
    EXPECT_EQ(controller->readDeviceState("SN-1", false), "UNKNOWN");  // 명부에 없어 실제로 실행됨
    EXPECT_EQ(runner.callsSnapshot().size(), 2u);
}

TEST_F(MatterControllerTest, PairingTimeoutWithSuccessTextIsFailureAndSavesNothing) {
    runner.respond = [](const std::vector<std::string>& sub) {
        if (startsWith(sub, {"pairing", "ble-wifi"})) return ProcessResult{128 + 15, "Device commissioning completed with success", true, false, false};
        return FakeChipToolOutput::ok("SerialNumber: SN-NEW");
    };
    EXPECT_FALSE(controller->commission(CommissionRequest{"plug", kGoodCode, "ssid", "pw"}, {}).ok());
    EXPECT_EQ(readConfig(), "SN-1,101\nSN-2,102\n");
    EXPECT_EQ(runner.callsSnapshot().size(), 1u);  // 시리얼 읽기로 넘어가지 않음
}

// 매핑 없는 시리얼은 제어·상태 읽기도 chip-tool을 실행하지 않는다
TEST_F(MatterControllerTest, UnknownSerialControlAndStateRunNothing) {
    EXPECT_FALSE(controller->sendCommand("NOPE", "ON"));
    EXPECT_EQ(controller->readDeviceState("NOPE", true), "UNKNOWN");
    EXPECT_TRUE(runner.callsSnapshot().empty());
}

// 등록 파일 쓰기는 같은 시리얼 줄을 하나로 바꾸고, 임시 파일을 남기지 않는다
TEST_F(MatterControllerTest, RegistrationWriteReplacesSameSerialAndLeavesNoTempFile) {
    runner.respond = [](const std::vector<std::string>& sub) {
        if (startsWith(sub, {"pairing", "ble-wifi"})) return FakeChipToolOutput::ok("Device commissioning completed with success");
        return FakeChipToolOutput::ok("SerialNumber: SN-1");  // 이미 101로 등록된 시리얼이 다시 페어링됨
    };
    const auto outcome = controller->commission(CommissionRequest{"plug", kGoodCode, "ssid", "pw"}, {});
    ASSERT_TRUE(outcome.ok());
    const std::string nodeId = runner.callsSnapshot()[0].args[3];

    EXPECT_EQ(readConfig(), "SN-2,102\nSN-1," + nodeId + "\n");
    EXPECT_FALSE(fs::exists(configPath_ + ".tmp"));
}

// ---- E9 + R7: DB 저장에 실패하면 Matter 쪽 등록(매핑·파일)까지 정리한다 ----

TEST_F(MatterControllerTest, DatabaseFailureAfterPairingCleansUpMatterRegistration) {
    const std::string dbPath = (dir_ / "test.db").string();
    DatabaseManager database;
    ASSERT_TRUE(database.init(dbPath));
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(dbPath.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw, "CREATE TRIGGER reject_device BEFORE INSERT ON devices BEGIN SELECT RAISE(ABORT, 'no'); END;",
                               nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close_v2(raw);
    }
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager(database, deviceDrivers, scheduler, clock);
    manager.registerDriver(ProtocolType::MATTER, controller.get());
    runner.respond = [](const std::vector<std::string>& sub) {
        if (startsWith(sub, {"pairing", "ble-wifi"})) return FakeChipToolOutput::ok("Device commissioning completed with success");
        if (startsWith(sub, {"basicinformation"})) return FakeChipToolOutput::ok("SerialNumber: SN-NEW");
        return FakeChipToolOutput::ok("");
    };

    EXPECT_FALSE(manager.startCommissioning(ProtocolType::MATTER, "plug", kGoodCode, "ssid", "pw"));
    ASSERT_TRUE(runner.waitUntilIdle(3));  // 페어링 + 시리얼 + 정리 unpair

    EXPECT_TRUE(manager.getAllDevices().empty());
    EXPECT_EQ(controller->readEnergy("SN-NEW").error.code, ErrorCode::DeviceNotFound);  // 매핑 없음
    EXPECT_EQ(readConfig(), "SN-1,101\nSN-2,102\n");                                     // 파일 줄 없음
    bool unpairRan = false;
    for (const auto& sub : subcommands()) unpairRan = unpairRan || startsWith(sub, {"pairing", "unpair"});
    EXPECT_TRUE(unpairRan);
    database.close();
}

// ---- E9: DB 저장에 실패하면 등록을 되돌리고 커미셔닝 실패 ----

class CommissionRollbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               (std::string("hac_rollback_test_") + std::to_string(::getpid()) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        dbPath_ = (dir_ / "test.db").string();
        ASSERT_TRUE(database.init(dbPath_));
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    }
    void TearDown() override {
        database.close();
        fs::remove_all(dir_);
    }
    void rejectDeviceInserts() {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(dbPath_.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw, "CREATE TRIGGER reject_device BEFORE INSERT ON devices BEGIN SELECT RAISE(ABORT, 'no'); END;"
                                    "CREATE TRIGGER reject_device_update BEFORE UPDATE ON devices BEGIN SELECT RAISE(ABORT, 'no'); END;",
                               nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close_v2(raw);
    }

    fs::path dir_;
    std::string dbPath_;
    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver tcpFake;
};

TEST_F(CommissionRollbackTest, DatabaseFailureUndoesRegistrationAndReportsFailure) {
    rejectDeviceInserts();
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"NEW_1"});

    EXPECT_FALSE(manager.startCommissioning(ProtocolType::TCP_DIY, "lamp", ""));

    EXPECT_TRUE(manager.getAllDevices().empty());
    EXPECT_TRUE(database.loadAllDevices().empty());
}

// DB를 먼저 쓰므로 실패하면 메모리는 처음부터 바뀌지 않는다(이전 기기 그대로)
TEST_F(CommissionRollbackTest, DatabaseFailureOnReRegistrationKeepsPreviousDevice) {
    ASSERT_TRUE(manager.addDevice("A", "old name", ProtocolType::TCP_DIY, {}, &tcpFake));
    rejectDeviceInserts();

    EXPECT_FALSE(manager.addDevice("A", "new name", ProtocolType::TCP_DIY, {}, &tcpFake));

    Device device;
    ASSERT_TRUE(manager.getDevice("A", device));
    EXPECT_EQ(device.name, "old name");
}

}  // namespace
