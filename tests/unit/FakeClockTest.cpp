#include "fakes/FakeClock.hpp"
#include <gtest/gtest.h>

TEST(FakeClockTest, AdvanceMovesBothClocks) {
    FakeClock clock;
    auto steadyBefore = clock.steadyNow();
    auto utcBefore = clock.utcNow();
    clock.advance(std::chrono::seconds(90));
    EXPECT_EQ(clock.steadyNow() - steadyBefore, std::chrono::seconds(90));
    EXPECT_EQ(clock.utcNow() - utcBefore, std::chrono::seconds(90));
}

TEST(FakeClockTest, StartTimeCanBeSet) {
    auto utcStart = std::chrono::system_clock::time_point{} + std::chrono::hours(1000);
    FakeClock clock({}, utcStart);
    EXPECT_EQ(clock.utcNow(), utcStart);
}

TEST(SystemClockTest, ReturnsRealTime) {
    SystemClock clock;
    EXPECT_LE(clock.steadyNow(), clock.steadyNow() + std::chrono::seconds(1));
    EXPECT_GT(clock.utcNow().time_since_epoch().count(), 0);
}
