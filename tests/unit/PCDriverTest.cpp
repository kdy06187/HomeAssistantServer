#include <gtest/gtest.h>
#include "PCDriver.hpp"
#include "Device.hpp"
#include "fakes/MockMessageSender.hpp"

using ::testing::_;
using ::testing::HasSubstr;
using ::testing::StrictMock;

namespace {
Device makeDeviceWithState(const std::string& state) {
    Device device("PC_1", "pc", ProtocolType::TCP_DIY);
    device.state = state;
    return device;
}

TEST(PCDriverHandleCommand, OffOnRunningDeviceSendsShutdownAndDelays45Seconds) {
    StrictMock<MockMessageSender> sender;
    EXPECT_CALL(sender, broadcast(HasSubstr("SHUTDOWN"))).Times(1);
    PCDriver driver(&sender);

    DriverResult result = driver.handleCommand(makeDeviceWithState("ON"), "OFF");

    EXPECT_TRUE(result.proceed_hardware);
    EXPECT_EQ(result.delay_seconds, 45);
}

TEST(PCDriverHandleCommand, OffOnAlreadyOffDeviceSendsNothingAndBlocksHardware) {
    StrictMock<MockMessageSender> sender;
    PCDriver driver(&sender);

    for (const char* state : {"OFF", "OFFLINE"}) {
        DriverResult result = driver.handleCommand(makeDeviceWithState(state), "OFF");
        EXPECT_FALSE(result.proceed_hardware) << state;
        EXPECT_EQ(result.delay_seconds, 0) << state;
    }
}

TEST(PCDriverHandleCommand, OnCommandDoesNotInterfere) {
    StrictMock<MockMessageSender> sender;
    PCDriver driver(&sender);

    DriverResult result = driver.handleCommand(makeDeviceWithState("OFF"), "ON");

    EXPECT_TRUE(result.proceed_hardware);
    EXPECT_EQ(result.delay_seconds, 0);
}

TEST(PCDriverHandleCommand, LowercaseOffIsNotIntercepted) {
    StrictMock<MockMessageSender> sender;
    PCDriver driver(&sender);

    DriverResult result = driver.handleCommand(makeDeviceWithState("ON"), "off");

    EXPECT_TRUE(result.proceed_hardware);
    EXPECT_EQ(result.delay_seconds, 0);
}

TEST(PCDriverHandleCommand, OffOnUnknownStateSendsShutdown) {
    StrictMock<MockMessageSender> sender;
    EXPECT_CALL(sender, broadcast(_)).Times(1);
    PCDriver driver(&sender);

    DriverResult result = driver.handleCommand(makeDeviceWithState("UNKNOWN"), "OFF");

    EXPECT_EQ(result.delay_seconds, 45);
}

TEST(PCDriverHandleCommand, OffWithoutSenderStillDelays) {
    PCDriver driver(nullptr);

    DriverResult result = driver.handleCommand(makeDeviceWithState("ON"), "OFF");

    EXPECT_TRUE(result.proceed_hardware);
    EXPECT_EQ(result.delay_seconds, 45);
}
}  // namespace
