#include <gtest/gtest.h>
#include "PeriodicTasks.hpp"

TEST(ShouldRunCleanupTest, NotBeforeTheSixtiethTick) {
    EXPECT_FALSE(shouldRunCleanup(0));
    EXPECT_FALSE(shouldRunCleanup(1));
    EXPECT_FALSE(shouldRunCleanup(59));
}

TEST(ShouldRunCleanupTest, FirstRunAtSixtiethTick) {
    EXPECT_TRUE(shouldRunCleanup(60));
    EXPECT_FALSE(shouldRunCleanup(61));
}

TEST(ShouldRunCleanupTest, RepeatsEvery1440TicksAfterTheFirstRun) {
    EXPECT_FALSE(shouldRunCleanup(1499));
    EXPECT_TRUE(shouldRunCleanup(1500));
    EXPECT_FALSE(shouldRunCleanup(1501));
    EXPECT_TRUE(shouldRunCleanup(60 + 1440 * 2));
}

TEST(ShouldRunCleanupTest, NegativeCountNeverRuns) {
    EXPECT_FALSE(shouldRunCleanup(-1));
}
