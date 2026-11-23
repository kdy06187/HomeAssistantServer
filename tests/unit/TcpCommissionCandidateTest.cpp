#include <gtest/gtest.h>
#include "TcpCommissionCandidate.hpp"

using TcpCommissionCandidate::pickNewDevice;

namespace {

const std::string kAgent = "TCP_AAAAAAAAAAAA";

TEST(TcpCommissionCandidateTest, PicksTheSingleNewIdentifiedDevice) {
    EXPECT_EQ(pickNewDevice({"TCP_000000000001"}, {"TCP_000000000001", "TCP_000000000002"}, {}, kAgent),
              "TCP_000000000002");
}

TEST(TcpCommissionCandidateTest, IgnoresIdsThatWereAlreadyConnectedBeforeCommissioning) {
    EXPECT_FALSE(pickNewDevice({"TCP_000000000001"}, {"TCP_000000000001"}, {}, kAgent));
}

TEST(TcpCommissionCandidateTest, IgnoresRegisteredDeviceThatReconnectedDuringWait) {
    // 시작 때는 끊겨 있다가 기다리는 동안 다시 붙은 기존 스위치
    EXPECT_FALSE(pickNewDevice({}, {"TCP_000000000003"}, {"TCP_000000000003"}, kAgent));
}

TEST(TcpCommissionCandidateTest, IgnoresPcAgent) {
    EXPECT_FALSE(pickNewDevice({}, {kAgent}, {}, kAgent));
    EXPECT_EQ(pickNewDevice({}, {kAgent}, {}, std::nullopt), kAgent);  // 설정이 없으면 에이전트를 알 수 없다
}

TEST(TcpCommissionCandidateTest, IgnoresIdsWithoutMacPrefix) {
    EXPECT_FALSE(pickNewDevice({}, {"ARD_192.168.0.5"}, {}, kAgent));
}

TEST(TcpCommissionCandidateTest, SeveralCandidatesGiveTheSameFirstOneRegardlessOfOrder) {
    EXPECT_EQ(pickNewDevice({}, {"TCP_00000000000B", "TCP_00000000000A"}, {}, kAgent), "TCP_00000000000A");
    EXPECT_EQ(pickNewDevice({}, {"TCP_00000000000A", "TCP_00000000000B"}, {}, kAgent), "TCP_00000000000A");
}

TEST(TcpCommissionCandidateTest, NothingConnectedMeansNoCandidate) {
    EXPECT_FALSE(pickNewDevice({}, {}, {}, kAgent));
}

}  // namespace
