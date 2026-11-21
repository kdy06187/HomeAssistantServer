#include <gtest/gtest.h>
#include "AuthFailureMonitor.hpp"
#include "fakes/FakeClock.hpp"

using namespace std::chrono_literals;

namespace {

TEST(AuthFailureMonitor, WarnsOnceWhenMoreThanTenFailuresInAMinute) {
    FakeClock clock;
    AuthFailureMonitor monitor(clock);
    for (int i = 1; i <= 10; ++i) EXPECT_FALSE(monitor.recordFailure()) << i;
    EXPECT_TRUE(monitor.recordFailure());   // 11번째
    EXPECT_FALSE(monitor.recordFailure());  // 같은 구간에서는 다시 경고하지 않는다
    EXPECT_FALSE(monitor.recordFailure());
}

TEST(AuthFailureMonitor, SlowFailuresNeverWarn) {
    FakeClock clock;
    AuthFailureMonitor monitor(clock);
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(monitor.recordFailure());
        clock.advance(7s);  // 1분에 약 8회
    }
}

TEST(AuthFailureMonitor, FailuresOlderThanAMinuteAreForgotten) {
    FakeClock clock;
    AuthFailureMonitor monitor(clock);
    for (int i = 0; i < 10; ++i) monitor.recordFailure();
    clock.advance(60s);
    EXPECT_FALSE(monitor.recordFailure());  // 이전 10건은 창 밖이라 1건으로 센다
}

TEST(AuthFailureMonitor, WarnsAgainAfterCalmingDown) {
    FakeClock clock;
    AuthFailureMonitor monitor(clock);
    for (int i = 0; i < 10; ++i) monitor.recordFailure();
    EXPECT_TRUE(monitor.recordFailure());
    clock.advance(61s);
    EXPECT_FALSE(monitor.recordFailure());  // 조용해짐 → 경고 상태 해제
    for (int i = 0; i < 9; ++i) EXPECT_FALSE(monitor.recordFailure());
    EXPECT_TRUE(monitor.recordFailure());   // 다시 11건
}

}  // namespace
