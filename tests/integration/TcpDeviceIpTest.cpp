#include <gtest/gtest.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "TcpServer.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeMacResolver.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "support/FakeTcpDevice.hpp"
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using fake_tcp::FakeDevice;

namespace {

// main과 같은 배선: TcpServer 접속 알림 → DeviceManager::onTcpConnected
TEST(TcpDeviceIpTest, RealConnectionUpdatesRegisteredDeviceIp) {
    DatabaseManager database;
    ASSERT_TRUE(database.init(":memory:"));
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver tcpFake;
    manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    const std::string id = FakeMacResolver::loopbackId(2);
    ASSERT_TRUE(manager.addDevice(id, "lamp", ProtocolType::TCP_DIY, {}, "192.168.0.50"));

    FakeMacResolver resolver;
    resolver.setMac("127.0.0.6", FakeMacResolver::loopbackMac(2));  // 같은 기기가 다른 IP로
    TcpServer server(resolver);
    server.setConnectionListener([&manager](const TcpConnectionInfo& info) { manager.onTcpConnected(info.deviceId, info.ip); });
    ASSERT_TRUE(server.start(0, "127.0.0.1"));
    auto ipOf = [&] {
        Device device;
        manager.getDevice(id, device);
        return device.ip;
    };
    auto waitForIp = [&](const std::string& expected) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (ipOf() != expected && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
        return ipOf() == expected;
    };

    FakeDevice first(server.boundPort(), 2);
    EXPECT_TRUE(waitForIp("127.0.0.2"));
    first.closeSocket();
    FakeDevice moved(server.boundPort(), 6);
    EXPECT_TRUE(waitForIp("127.0.0.6"));
    for (const auto& device : database.loadActiveDevices()) {
        if (device.id == id) {
            EXPECT_EQ(device.ip, "127.0.0.6");
        }
    }
    server.stop();
}

}  // namespace
