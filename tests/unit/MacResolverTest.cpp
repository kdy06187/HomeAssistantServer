#include <gtest/gtest.h>
#include "MacResolver.hpp"
#include <cstdio>
#include <fstream>
#include <string>
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

TEST(MacResolverParseRoute, FindsDefaultGatewayInLittleEndianHex) {
    const std::string route = std::string(kRouteHeader) +
                              "wlan0\t0000A8C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n" +
                              "wlan0\t00000000\t0100A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n";
    EXPECT_EQ(findDefaultGatewayIp(route), "192.168.0.1");
}

TEST(MacResolverParseRoute, NoDefaultRouteOrNoGatewayFlag) {
    EXPECT_FALSE(findDefaultGatewayIp(kRouteHeader));
    const std::string noGatewayFlag = std::string(kRouteHeader) +
                                      "eth0\t00000000\t0101A8C0\t0001\t0\t0\t0\t00000000\t0\t0\t0\n";
    EXPECT_FALSE(findDefaultGatewayIp(noGatewayFlag));
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
