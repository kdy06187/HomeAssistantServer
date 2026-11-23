#include <gtest/gtest.h>
#include "MacResolver.hpp"
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace MacResolverParse;

namespace {

const char* kArpHeader = "IP address       HW type     Flags       HW address            Mask     Device\n";

std::string arpLine(const std::string& ip, const std::string& flags, const std::string& mac,
                    const std::string& dev = "wlan0") {
    return ip + "     0x1         " + flags + "         " + mac + "     *        " + dev + "\n";
}

const char* kRouteHeader = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n";

TEST(MacResolverParseNormalize, AcceptsColonHyphenAndBareForms) {
    EXPECT_EQ(normalizeMac("aa:bb:cc:dd:ee:ff"), "AABBCCDDEEFF");
    EXPECT_EQ(normalizeMac("AA-BB-CC-DD-EE-0f"), "AABBCCDDEE0F");
    EXPECT_EQ(normalizeMac("a1b2c3d4e5f6"), "A1B2C3D4E5F6");
}

TEST(MacResolverParseNormalize, RejectsWrongLengthMixedSeparatorsAndNonHex) {
    EXPECT_FALSE(normalizeMac("A1B2C3D4E5F"));          // 11자
    EXPECT_FALSE(normalizeMac("A1B2C3D4E5F6A"));        // 13자
    EXPECT_FALSE(normalizeMac("A1B2C3D4E5G6"));         // 16진 아님
    EXPECT_FALSE(normalizeMac("aa:bb-cc:dd:ee:ff"));    // 구분자 섞임
    EXPECT_FALSE(normalizeMac("aa.bb.cc.dd.ee.ff"));    // 허용 안 된 구분자
    EXPECT_FALSE(normalizeMac("aab:bcc:dde:eff:0:1"));  // 구분자 위치 틀림
    EXPECT_FALSE(normalizeMac(""));
}

TEST(MacResolverParseArp, FindsCompleteEntryByExactIp) {
    const std::string arp = std::string(kArpHeader) + arpLine("192.168.0.10", "0x2", "a1:b2:c3:d4:e5:f6") +
                            arpLine("192.168.0.1", "0x2", "11:22:33:44:55:66");
    EXPECT_EQ(findArpMac(arp, "192.168.0.10"), "A1B2C3D4E5F6");
    EXPECT_EQ(findArpMac(arp, "192.168.0.1"), "112233445566");  // 앞부분이 같은 .10과 섞이지 않음
    EXPECT_FALSE(findArpMac(arp, "192.168.0.100"));
}

TEST(MacResolverParseArp, IncompleteEntryIsNotFound) {
    const std::string arp = std::string(kArpHeader) + arpLine("192.168.0.20", "0x0", "00:00:00:00:00:00");
    EXPECT_FALSE(findArpMac(arp, "192.168.0.20"));
}

TEST(MacResolverParseArp, SkipsIncompleteAndUsesCompleteOnOtherDevice) {
    const std::string arp = std::string(kArpHeader) + arpLine("10.0.0.5", "0x0", "00:00:00:00:00:00", "eth0") +
                            arpLine("10.0.0.5", "0x2", "de:ad:be:ef:00:01", "wlan0");
    EXPECT_EQ(findArpMac(arp, "10.0.0.5"), "DEADBEEF0001");
}

TEST(MacResolverParseArp, EmptyTableOrHeaderOnly) {
    EXPECT_FALSE(findArpMac("", "192.168.0.1"));
    EXPECT_FALSE(findArpMac(kArpHeader, "192.168.0.1"));
}

std::string routeLine(const std::string& iface, const std::string& dest, const std::string& gw, const std::string& flags,
                      const std::string& mask) {
    return iface + "\t" + dest + "\t" + gw + "\t" + flags + "\t0\t0\t600\t" + mask + "\t0\t0\t0\n";
}

TEST(MacResolverParseRoute, FindsDefaultGatewayInKernelByteOrder) {
    const std::string route = std::string(kRouteHeader) + routeLine("wlan0", "0000A8C0", "00000000", "0001", "00FFFFFF") +
                              routeLine("wlan0", "00000000", "0100A8C0", "0003", "00000000");
    EXPECT_EQ(findDefaultGatewayIps(route), std::vector<std::string>{"192.168.0.1"});
}

TEST(MacResolverParseRoute, NoDefaultRouteOrMissingUpOrGatewayFlag) {
    EXPECT_TRUE(findDefaultGatewayIps(kRouteHeader).empty());
    EXPECT_TRUE(findDefaultGatewayIps(std::string(kRouteHeader) + routeLine("eth0", "00000000", "0101A8C0", "0001", "00000000")).empty());
    EXPECT_TRUE(findDefaultGatewayIps(std::string(kRouteHeader) + routeLine("eth0", "00000000", "0101A8C0", "0002", "00000000")).empty());
}

// VPN이 까는 0.0.0.0/1 같은 반쪽 경로는 목적지가 0이어도 마스크가 0이 아니다
TEST(MacResolverParseRoute, HalfRouteWithNonZeroMaskIsNotDefault) {
    const std::string route = std::string(kRouteHeader) + routeLine("tun0", "00000000", "0101000A", "0003", "00000080");
    EXPECT_TRUE(findDefaultGatewayIps(route).empty());
}

TEST(MacResolverParseRoute, MalformedHexFieldsAreSkipped) {
    for (const char* bad : {"0100A8ZZ", "-1", "0x0100A8", "100A8C0", "0100A8C00"}) {
        const std::string route = std::string(kRouteHeader) + routeLine("wlan0", "00000000", bad, "0003", "00000000");
        EXPECT_TRUE(findDefaultGatewayIps(route).empty()) << bad;
    }
    const std::string badFlags = std::string(kRouteHeader) + routeLine("wlan0", "00000000", "0100A8C0", "0x03", "00000000");
    EXPECT_TRUE(findDefaultGatewayIps(badFlags).empty());
}

TEST(MacResolverParseRoute, ReturnsEveryDefaultGateway) {
    const std::string route = std::string(kRouteHeader) + routeLine("wlan0", "00000000", "0100A8C0", "0003", "00000000") +
                              routeLine("eth0", "00000000", "0101A8C0", "0003", "00000000");
    EXPECT_EQ(findDefaultGatewayIps(route), (std::vector<std::string>{"192.168.0.1", "192.168.1.1"}));
}

// 실제 구현은 고정 내용 파일로 시험한다(재시도 간격 0)
class ProcNetArpResolverTest : public ::testing::Test {
protected:
    void SetUp() override {
        arpPath = "/tmp/hac_arp_" + std::to_string(::getpid());
        routePath = "/tmp/hac_route_" + std::to_string(::getpid());
    }
    void TearDown() override {
        std::remove(arpPath.c_str());
        std::remove(routePath.c_str());
    }
    void write(const std::string& path, const std::string& content) {
        std::ofstream(path) << content;
    }
    std::string arpPath, routePath;
};

TEST_F(ProcNetArpResolverTest, ReturnsMacOfLanPeer) {
    write(arpPath, std::string(kArpHeader) + arpLine("192.168.0.1", "0x2", "11:22:33:44:55:66") +
                       arpLine("192.168.0.50", "0x2", "a1:b2:c3:d4:e5:f6"));
    write(routePath, std::string(kRouteHeader) + "wlan0\t00000000\t0100A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n");
    ProcNetArpResolver resolver(arpPath, routePath, 3, std::chrono::milliseconds(0));
    EXPECT_EQ(resolver.resolve("192.168.0.50"), "A1B2C3D4E5F6");
}

TEST_F(ProcNetArpResolverTest, PeerWithGatewayMacIsBehindRouter) {
    // 다른 서브넷 접속을 라우터가 넘기면 ARP 표에 라우터 MAC으로 보인다
    write(arpPath, std::string(kArpHeader) + arpLine("192.168.0.1", "0x2", "11:22:33:44:55:66") +
                       arpLine("192.168.0.77", "0x2", "11:22:33:44:55:66"));
    write(routePath, std::string(kRouteHeader) + "wlan0\t00000000\t0100A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n");
    ProcNetArpResolver resolver(arpPath, routePath, 3, std::chrono::milliseconds(0));
    EXPECT_FALSE(resolver.resolve("192.168.0.77"));
}

// 기본 게이트웨이가 여러 개(유선·무선)면 어느 쪽 MAC이어도 거부한다
TEST_F(ProcNetArpResolverTest, PeerWithSecondGatewayMacIsAlsoRejected) {
    write(arpPath, std::string(kArpHeader) + arpLine("192.168.0.1", "0x2", "11:22:33:44:55:66") +
                       arpLine("192.168.1.1", "0x2", "aa:aa:aa:aa:aa:01", "eth0") +
                       arpLine("192.168.1.77", "0x2", "aa:aa:aa:aa:aa:01", "eth0"));
    write(routePath, std::string(kRouteHeader) + routeLine("wlan0", "00000000", "0100A8C0", "0003", "00000000") +
                         routeLine("eth0", "00000000", "0101A8C0", "0003", "00000000"));
    ProcNetArpResolver resolver(arpPath, routePath, 1, std::chrono::milliseconds(0));
    EXPECT_FALSE(resolver.resolve("192.168.1.77"));
}

// 핸드셰이크 직후 ARP 항목이 아직 없다가 생기면 재시도에서 찾는다. 시도 1번이면 못 찾는다
TEST_F(ProcNetArpResolverTest, RetryFindsEntryThatAppearsLater) {
    write(arpPath, kArpHeader);
    ProcNetArpResolver once(arpPath, routePath, 1, std::chrono::milliseconds(0));
    EXPECT_FALSE(once.resolve("192.168.0.50"));

    ProcNetArpResolver retrying(arpPath, routePath, 3, std::chrono::milliseconds(150));
    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 첫 시도 뒤, 두 번째 시도 전
        write(arpPath, std::string(kArpHeader) + arpLine("192.168.0.50", "0x2", "a1:b2:c3:d4:e5:f6"));
    });
    const auto found = retrying.resolve("192.168.0.50");
    writer.join();
    EXPECT_EQ(found, "A1B2C3D4E5F6");
}

TEST_F(ProcNetArpResolverTest, MissingFilesOrEntryIsNotFound) {
    ProcNetArpResolver missing("/nonexistent/arp", "/nonexistent/route", 2, std::chrono::milliseconds(0));
    EXPECT_FALSE(missing.resolve("192.168.0.50"));
    write(arpPath, kArpHeader);
    ProcNetArpResolver empty(arpPath, routePath, 2, std::chrono::milliseconds(0));
    EXPECT_FALSE(empty.resolve("192.168.0.50"));
}

TEST_F(ProcNetArpResolverTest, WorksWithoutRouteFile) {
    write(arpPath, std::string(kArpHeader) + arpLine("192.168.0.50", "0x2", "a1:b2:c3:d4:e5:f6"));
    ProcNetArpResolver resolver(arpPath, "/nonexistent/route", 1, std::chrono::milliseconds(0));
    EXPECT_EQ(resolver.resolve("192.168.0.50"), "A1B2C3D4E5F6");
}

TEST(MakeTcpDeviceId, PrefixesMac) {
    EXPECT_EQ(makeTcpDeviceId("A1B2C3D4E5F6"), "TCP_A1B2C3D4E5F6");
}

}  // namespace
