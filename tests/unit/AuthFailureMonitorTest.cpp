#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
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

TEST(AuthFailureMonitor, LongBurstStillWarnsOnceAndForgetsAfterWindow) {
    FakeClock clock;
    AuthFailureMonitor monitor(clock);
    int warnings = 0;
    for (int i = 0; i < 100000; ++i) warnings += monitor.recordFailure() ? 1 : 0;
    EXPECT_EQ(warnings, 1);
    clock.advance(61s);
    EXPECT_FALSE(monitor.recordFailure());
}

TEST(AuthFailureMonitor, ConcurrentCallsWarnExactlyOnce) {
    FakeClock clock;
    AuthFailureMonitor monitor(clock);
    std::atomic<int> warnings{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 500; ++i) {
                if (monitor.recordFailure()) ++warnings;
                if (i % 100 == 0) clock.advance(1ms);  // 시계 접근도 겹치게
            }
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(warnings.load(), 1);
}

}  // namespace
