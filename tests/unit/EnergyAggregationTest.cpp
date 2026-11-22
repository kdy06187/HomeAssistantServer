#include <gtest/gtest.h>
#include "EnergyAggregation.hpp"

namespace {

using namespace EnergyAggregation;

EnergyLog sampleAt(const std::string& utcText, int64_t total) { return EnergyLog{utcText, total}; }

// 현지 날짜의 정오(+09:00) = 같은 날 03:00 UTC
EnergyLog localNoon(const CivilDate& date, int64_t total) { return sampleAt(formatDate(date) + " 03:00:00", total); }

std::string span(const EnergyBucket& bucket) { return formatDate(bucket.start) + "~" + formatDate(bucket.end); }

}  // namespace

TEST(EnergyAggregationDateTest, KnownDayNumbers) {
    EXPECT_EQ(daysFromCivil({1970, 1, 1}), 0);
    EXPECT_EQ(daysFromCivil({1969, 12, 31}), -1);
    EXPECT_EQ(daysFromCivil({2000, 3, 1}), 11017);
    EXPECT_EQ(daysFromCivil({2028, 3, 1}) - daysFromCivil({2028, 2, 28}), 2); // 윤년
    EXPECT_EQ(daysFromCivil({2100, 3, 1}) - daysFromCivil({2100, 2, 28}), 1); // 100년 단위는 평년
}

TEST(EnergyAggregationDateTest, CivilFromDaysInvertsDaysFromCivil) {
    for (int64_t day = -800; day < 60000; day += 7) {
        const CivilDate date = civilFromDays(day);
        ASSERT_EQ(daysFromCivil(date), day) << formatDate(date);
    }
    const CivilDate leapDay = civilFromDays(daysFromCivil({2028, 2, 29}));
    EXPECT_EQ(formatDate(leapDay), "2028-02-29");
}

TEST(EnergyAggregationDateTest, LocalDateRangeStartsNineHoursEarlierInUtc) {
    const auto range = localDateRangeToUtc({2026, 11, 1}, {2026, 11, 1});
    EXPECT_EQ(range.fromUtc, "2026-10-31 15:00:00");
    EXPECT_EQ(range.toUtc, "2026-11-01 15:00:00"); // 끝 날짜의 다음 날 0시 미만
}

TEST(EnergyAggregationDateTest, LocalDateRangeAcrossYearEnd) {
    const auto range = localDateRangeToUtc({2026, 12, 31}, {2027, 1, 1});
    EXPECT_EQ(range.fromUtc, "2026-12-30 15:00:00");
    EXPECT_EQ(range.toUtc, "2027-01-01 15:00:00");
}

// 하루에 100씩 늘어나는 기록: 구간 사용량 = 100 x (그 구간에서 뒤 기록이 속한 일 수)
TEST(EnergyAggregationTest, WeekBucketsMatchContractExample) {
    std::vector<EnergyLog> samples{localNoon({2026, 10, 31}, 0)}; // 범위 시작 직전 1건
    for (int day = 1; day <= 17; ++day) samples.push_back(localNoon({2026, 11, day}, day * 100));

    const auto buckets = aggregate(samples, {2026, 11, 1}, {2026, 11, 17}, HistoryPeriod::Week);

    ASSERT_EQ(buckets.size(), 4u);
    EXPECT_EQ(span(buckets[0]), "2026-11-01~2026-11-01"); // 일요일이라 첫 주는 하루
    EXPECT_EQ(span(buckets[1]), "2026-11-02~2026-11-08");
    EXPECT_EQ(span(buckets[2]), "2026-11-09~2026-11-15");
    EXPECT_EQ(span(buckets[3]), "2026-11-16~2026-11-17"); // 끝 구간은 to로 잘림
    EXPECT_EQ(buckets[0].energyUsed_mWh, 100);
    EXPECT_EQ(buckets[1].energyUsed_mWh, 700);
    EXPECT_EQ(buckets[2].energyUsed_mWh, 700);
    EXPECT_EQ(buckets[3].energyUsed_mWh, 200);
    EXPECT_EQ(buckets[0].sampleCount, 1);
    EXPECT_EQ(buckets[1].sampleCount, 7);
    EXPECT_EQ(buckets[3].sampleCount, 2);
}

TEST(EnergyAggregationTest, WeekStartsOnMondayAcrossYearEnd) {
    const auto buckets = aggregate({}, {2026, 12, 28}, {2027, 1, 5}, HistoryPeriod::Week);
    ASSERT_EQ(buckets.size(), 2u);
    EXPECT_EQ(span(buckets[0]), "2026-12-28~2027-01-03");
    EXPECT_EQ(span(buckets[1]), "2027-01-04~2027-01-05");
}

TEST(EnergyAggregationTest, MonthBucketsFollowCalendarIncludingLeapFebruary) {
    const auto buckets = aggregate({}, {2028, 1, 15}, {2028, 3, 10}, HistoryPeriod::Month);
    ASSERT_EQ(buckets.size(), 3u);
    EXPECT_EQ(span(buckets[0]), "2028-01-15~2028-01-31");
    EXPECT_EQ(span(buckets[1]), "2028-02-01~2028-02-29");
    EXPECT_EQ(span(buckets[2]), "2028-03-01~2028-03-10");
}

TEST(EnergyAggregationTest, MonthBucketsAcrossYearEnd) {
    const auto buckets = aggregate({}, {2026, 12, 1}, {2027, 1, 31}, HistoryPeriod::Month);
    ASSERT_EQ(buckets.size(), 2u);
    EXPECT_EQ(span(buckets[0]), "2026-12-01~2026-12-31");
    EXPECT_EQ(span(buckets[1]), "2027-01-01~2027-01-31");
}

TEST(EnergyAggregationTest, DayBucketsAroundLeapDay) {
    const auto buckets = aggregate({}, {2028, 2, 27}, {2028, 3, 1}, HistoryPeriod::Day);
    ASSERT_EQ(buckets.size(), 4u);
    EXPECT_EQ(span(buckets[2]), "2028-02-29~2028-02-29");
}

TEST(EnergyAggregationTest, FullLeapYearHas366DayBuckets) {
    EXPECT_EQ(aggregate({}, {2028, 1, 1}, {2028, 12, 31}, HistoryPeriod::Day).size(), 366u);
}

// UTC 15:00이 현지 자정이다. 1초 차이로 다른 날에 들어간다
TEST(EnergyAggregationTest, UtcFifteenOClockIsLocalMidnightBoundary) {
    const std::vector<EnergyLog> samples{
        sampleAt("2026-11-01 14:59:59", 100), // 현지 11-01 23:59:59
        sampleAt("2026-11-01 15:00:00", 150), // 현지 11-02 00:00:00
    };

    const auto buckets = aggregate(samples, {2026, 11, 1}, {2026, 11, 2}, HistoryPeriod::Day);

    ASSERT_EQ(buckets.size(), 2u);
    EXPECT_EQ(buckets[0].sampleCount, 1);
    EXPECT_EQ(buckets[0].energyUsed_mWh, 0);
    EXPECT_EQ(buckets[1].sampleCount, 1);
    EXPECT_EQ(buckets[1].energyUsed_mWh, 50); // 뒤 기록이 속한 날에 귀속
}

TEST(EnergyAggregationTest, DecreasingTotalCountsAsZeroUsage) {
    const std::vector<EnergyLog> samples{
        localNoon({2026, 11, 1}, 100), sampleAt("2026-11-01 04:00:00", 50), sampleAt("2026-11-01 05:00:00", 80)};

    const auto buckets = aggregate(samples, {2026, 11, 1}, {2026, 11, 1}, HistoryPeriod::Day);

    ASSERT_EQ(buckets.size(), 1u);
    EXPECT_EQ(buckets[0].energyUsed_mWh, 30); // 100->50은 0, 50->80은 30
    EXPECT_EQ(buckets[0].sampleCount, 3);
}

TEST(EnergyAggregationTest, SampleBeforeRangeGivesFirstBucketItsUsageButIsNotCounted) {
    const std::vector<EnergyLog> samples{
        localNoon({2026, 10, 31}, 1000), localNoon({2026, 11, 1}, 1500), localNoon({2026, 11, 3}, 9999)};

    const auto buckets = aggregate(samples, {2026, 11, 1}, {2026, 11, 2}, HistoryPeriod::Day);

    ASSERT_EQ(buckets.size(), 2u);
    EXPECT_EQ(buckets[0].energyUsed_mWh, 500);
    EXPECT_EQ(buckets[0].sampleCount, 1);
    EXPECT_EQ(buckets[1].sampleCount, 0); // 범위 밖 기록은 무시
}

TEST(EnergyAggregationTest, UsageGoesToBucketOfTheLaterSample) {
    const std::vector<EnergyLog> samples{localNoon({2026, 11, 8}, 100), localNoon({2026, 11, 9}, 400)};

    const auto buckets = aggregate(samples, {2026, 11, 2}, {2026, 11, 15}, HistoryPeriod::Week);

    ASSERT_EQ(buckets.size(), 2u);
    EXPECT_EQ(buckets[0].energyUsed_mWh, 0);   // 일요일 기록 1건, 앞 기록 없음
    EXPECT_EQ(buckets[1].energyUsed_mWh, 300); // 월요일 기록이 차이를 가져감
}

TEST(EnergyAggregationTest, EmptySamplesGiveBucketsWithZeroCount) {
    const auto buckets = aggregate({}, {2026, 11, 1}, {2026, 11, 3}, HistoryPeriod::Day);
    ASSERT_EQ(buckets.size(), 3u);
    for (const auto& bucket : buckets) {
        EXPECT_EQ(bucket.sampleCount, 0);
        EXPECT_EQ(bucket.energyUsed_mWh, 0);
    }
}
