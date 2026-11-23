#include <gtest/gtest.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "MatterController.hpp"
#include "PCDriver.hpp"
#include "fakes/FakeChipToolRunner.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include <unistd.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

// Matter 플러그에 PC_DRIVER를 붙였을 때 OFF가 "에이전트에 종료 메시지 1회 → 45초 뒤 전원 차단"인지 지킨다.
// 기기 드라이버 부착이 프로토콜 드라이버의 종류(메시지 발송 가능 여부)에 기대면,
// Matter 쪽에서 그 능력을 빼는 순간 PC 종료 없이 바로 전원이 꺼진다.
class MatterPcDriverRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               (std::string("hac_matter_pc_") + std::to_string(::getpid()) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        const std::string configPath = (dir_ / "registered_devices.txt").string();
        std::ofstream(configPath) << "SN-1,101\n";
        matter = std::make_unique<MatterController>(runner, [](const std::string&) {
            return Result<uint32_t>::success(20202021);
        }, "/fake/chip-tool", configPath, (dir_ / "chip-tool").string());
        ASSERT_TRUE(matter->Initialize());
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::MATTER, matter.get());
        manager.registerDriver(ProtocolType::TCP_DIY, &pcMessageSink);
        // main과 같게: PC 종료 메시지 발송기는 기기 프로토콜과 무관하게 팩토리가 쥔다
        deviceDrivers.registerDriver(DriverType::PC_DRIVER, [this]() {
            return std::make_shared<PCDriver>(&pcMessageSink);
        });
    }
    void TearDown() override {
        matter.reset();
        fs::remove_all(dir_);
    }

    void expectOffIsDeferredWithOneShutdownMessage(const std::string& id) {
        ASSERT_TRUE(manager.updateDeviceState(id, "ON"));
        const CommandResult result = manager.executeCommand(id, "OFF");
        EXPECT_EQ(result.outcome, CommandOutcome::Accepted);
        EXPECT_EQ(result.delaySeconds, 45);
        ASSERT_EQ(pcMessageSink.pcAgentMessages.size(), 1u);
        EXPECT_NE(pcMessageSink.pcAgentMessages[0].find("SHUTDOWN"), std::string::npos);
        for (const auto& call : runner.callsSnapshot()) {
            EXPECT_EQ(std::find(call.args.begin(), call.args.end(), "off"), call.args.end()) << "전원이 바로 꺼지면 안 된다";
        }
    }

    fs::path dir_;
    FakeChipToolRunner runner;
    std::unique_ptr<MatterController> matter;
    FakeProtocolDriver pcMessageSink;
    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
};

TEST_F(MatterPcDriverRegressionTest, MatterDeviceLoadedFromDatabaseKeepsPcShutdownSequence) {
    Device stored("SN-1", "plug", ProtocolType::MATTER);
    stored.driver_types = {DriverType::PC_DRIVER};
    ASSERT_TRUE(database.insertDevice(stored));
    manager.initFromDatabase();

    Device loaded;
    ASSERT_TRUE(manager.getDevice("SN-1", loaded));
    ASSERT_EQ(loaded.attached_drivers.size(), 1u);
    expectOffIsDeferredWithOneShutdownMessage("SN-1");
}

TEST_F(MatterPcDriverRegressionTest, MatterDeviceCommissionedWithPcDriverKeepsPcShutdownSequence) {
    runner.respond = [](const std::vector<std::string>& sub) {
        if (sub.size() >= 2 && sub[0] == "pairing") return FakeChipToolOutput::ok("Device commissioning completed with success");
        if (!sub.empty() && sub[0] == "basicinformation") return FakeChipToolOutput::ok("SerialNumber: SN-NEW");
        return FakeChipToolOutput::ok("");
    };
    ASSERT_TRUE(manager.startCommissioning(ProtocolType::MATTER, "plug", "code", "ssid", "pw", {DriverType::PC_DRIVER}));

    Device registered;
    ASSERT_TRUE(manager.getDevice("SN-NEW", registered));
    ASSERT_EQ(registered.attached_drivers.size(), 1u);
    expectOffIsDeferredWithOneShutdownMessage("SN-NEW");
}

}  // namespace
