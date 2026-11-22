#include <gtest/gtest.h>
#include "UtcTime.hpp"

namespace {
using std::chrono::seconds;
using Clock = std::chrono::system_clock;
}

TEST(UtcTimeTest, EpochFormatsAsDbText) {
    EXPECT_EQ(UtcTime::toDbText(Clock::time_point{}), "1970-01-01 00:00:00");
}

TEST(UtcTimeTest, RoundTripKeepsEveryField) {
    for (const char* text : {"2026-11-16 00:20:00", "2028-02-29 23:59:59", "2026-12-31 23:59:59", "2027-01-01 00:00:00"}) {
        const auto parsed = UtcTime::fromDbText(text);
        ASSERT_TRUE(parsed.has_value()) << text;
        EXPECT_EQ(UtcTime::toDbText(*parsed), text);
    }
}

TEST(UtcTimeTest, LeapDayExistsOnlyInLeapYears) {
    EXPECT_TRUE(UtcTime::fromDbText("2028-02-29 00:00:00").has_value());
    EXPECT_FALSE(UtcTime::fromDbText("2027-02-29 00:00:00").has_value());
    EXPECT_FALSE(UtcTime::fromDbText("2100-02-29 00:00:00").has_value());
    EXPECT_TRUE(UtcTime::fromDbText("2000-02-29 00:00:00").has_value());
}

TEST(UtcTimeTest, MidnightRolloverByAddingOneSecond) {
    const auto lastSecond = *UtcTime::fromDbText("2026-12-31 23:59:59");
    EXPECT_EQ(UtcTime::toDbText(lastSecond + seconds(1)), "2027-01-01 00:00:00");
}

TEST(UtcTimeTest, SubSecondIsFlooredIncludingBeforeEpoch) {
    EXPECT_EQ(UtcTime::toDbText(Clock::time_point{} + std::chrono::milliseconds(1999)), "1970-01-01 00:00:01");
    EXPECT_EQ(UtcTime::toDbText(Clock::time_point{} - std::chrono::milliseconds(1)), "1969-12-31 23:59:59");
}

TEST(UtcTimeTest, RejectsMalformedText) {
    for (const char* text : {"", "2026-11-16", "2026-11-16T00:20:00", "2026-11-16 24:00:00", "2026-13-01 00:00:00",
                             "2026-11-31 00:00:00", "2026-11-16 00:60:00", "2026-11-16 00:20:0x", "2026-11-16 00:20:00Z"}) {
        EXPECT_FALSE(UtcTime::fromDbText(text).has_value()) << text;
    }
}

TEST(UtcTimeTest, YearOutsideSafeRangeFails) {
    for (const char* text : {"2300-01-01 00:00:00", "1600-01-01 00:00:00", "1899-12-31 23:59:59", "2201-01-01 00:00:00",
                             "9999-12-31 23:59:59", "0000-01-01 00:00:00"}) {
        EXPECT_FALSE(UtcTime::fromDbText(text).has_value()) << text;
        EXPECT_FALSE(UtcTime::toEpochSeconds(text).has_value()) << text;
    }
    EXPECT_TRUE(UtcTime::fromDbText("1900-01-01 00:00:00").has_value());
    EXPECT_TRUE(UtcTime::fromDbText("2200-12-31 23:59:59").has_value());
}

TEST(UtcTimeTest, EpochSecondsRoundTripWithoutTimePoint) {
    EXPECT_EQ(UtcTime::toEpochSeconds("1970-01-02 00:00:00"), 86400);
    EXPECT_EQ(UtcTime::toDbTextFromEpochSeconds(86400), "1970-01-02 00:00:00");
    EXPECT_EQ(UtcTime::toDbTextFromEpochSeconds(-1), "1969-12-31 23:59:59");
}

TEST(UtcTimeTest, Iso8601AddsTAndZ) {
    EXPECT_EQ(UtcTime::toIso8601("2026-11-16 00:20:00"), "2026-11-16T00:20:00Z");
    EXPECT_EQ(UtcTime::toIso8601("garbage"), "garbage");
}
