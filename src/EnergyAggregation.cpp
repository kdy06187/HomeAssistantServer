#include "EnergyAggregation.hpp"
#include <algorithm>
#include <cstdio>
#include <optional>
#include "UtcTime.hpp"

namespace {

constexpr int64_t kSecondsPerDay = 86400;
constexpr int64_t kLocalOffsetSeconds = EnergyAggregation::kUtcOffsetHours * 3600;

struct DayRange {
    int64_t startDay;
    int64_t endDay; // 포함
};

int64_t floorDivide(int64_t value, int64_t divisor) {
    const int64_t quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

// 1970-01-01(목요일)을 기준으로 월요일=0
int weekdayFromMonday(int64_t days) {
    return static_cast<int>(((days + 3) % 7 + 7) % 7);
}

int64_t lastDayOfMonth(int64_t days) {
    const CivilDate date = EnergyAggregation::civilFromDays(days);
    const CivilDate nextMonthFirst = date.month == 12 ? CivilDate{date.year + 1, 1, 1}
                                                      : CivilDate{date.year, date.month + 1, 1};
    return EnergyAggregation::daysFromCivil(nextMonthFirst) - 1;
}

// 구간이 끝나는 날(첫·끝 구간은 범위 안으로 잘린다)
int64_t periodEndDay(int64_t startDay, int64_t lastDay, HistoryPeriod period) {
    int64_t naturalEnd = startDay;
    if (period == HistoryPeriod::Week) naturalEnd = startDay + (6 - weekdayFromMonday(startDay));
    if (period == HistoryPeriod::Month) naturalEnd = lastDayOfMonth(startDay);
    return std::min(naturalEnd, lastDay);
}

std::vector<DayRange> splitIntoPeriods(int64_t firstDay, int64_t lastDay, HistoryPeriod period) {
    std::vector<DayRange> ranges;
    for (int64_t start = firstDay; start <= lastDay;) {
        const int64_t end = periodEndDay(start, lastDay, period);
        ranges.push_back({start, end});
        start = end + 1;
    }
    return ranges;
}

// 기록 시각(UTC)이 속한 현지 날짜의 일 수
std::optional<int64_t> localDayOf(const std::string& dbText) {
    const auto epochSeconds = UtcTime::toEpochSeconds(dbText);
    if (!epochSeconds) return std::nullopt;
    return floorDivide(*epochSeconds + kLocalOffsetSeconds, kSecondsPerDay);
}

// day를 포함하는 구간의 위치. 구간은 빈틈없이 이어져 있다
size_t bucketIndexOf(const std::vector<DayRange>& ranges, int64_t day) {
    const auto next = std::upper_bound(ranges.begin(), ranges.end(), day,
        [](int64_t value, const DayRange& range) { return value < range.startDay; });
    return static_cast<size_t>(next - ranges.begin()) - 1;
}

}  // namespace

int64_t EnergyAggregation::daysFromCivil(const CivilDate& date) {
    const int64_t year = date.month <= 2 ? date.year - 1 : date.year;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yearOfEra = year - era * 400;
    const int64_t monthFromMarch = date.month > 2 ? date.month - 3 : date.month + 9;
    const int64_t dayOfYear = (153 * monthFromMarch + 2) / 5 + date.day - 1;
    const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

CivilDate EnergyAggregation::civilFromDays(int64_t days) {
    const int64_t shifted = days + 719468;
    const int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const int64_t dayOfEra = shifted - era * 146097;
    const int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const int64_t monthFromMarch = (5 * dayOfYear + 2) / 153;
    CivilDate date;
    date.day = static_cast<int>(dayOfYear - (153 * monthFromMarch + 2) / 5 + 1);
    date.month = static_cast<int>(monthFromMarch < 10 ? monthFromMarch + 3 : monthFromMarch - 9);
    date.year = static_cast<int>(yearOfEra + era * 400 + (date.month <= 2 ? 1 : 0));
    return date;
}

std::string EnergyAggregation::formatDate(const CivilDate& date) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", date.year, date.month, date.day);
    return buffer;
}

UtcRange EnergyAggregation::localDateRangeToUtc(const CivilDate& from, const CivilDate& to) {
    // 현지 날짜 D의 0시 = UTC로는 같은 날 0시보다 9시간 이르다
    const auto utcTextOfLocalMidnight = [](int64_t day) {
        return UtcTime::toDbTextFromEpochSeconds(day * kSecondsPerDay - kLocalOffsetSeconds);
    };
    return UtcRange{utcTextOfLocalMidnight(daysFromCivil(from)), utcTextOfLocalMidnight(daysFromCivil(to) + 1)};
}

std::vector<EnergyBucket> EnergyAggregation::aggregate(const std::vector<EnergyLog>& samples, const CivilDate& from,
                                                       const CivilDate& to, HistoryPeriod period) {
    const int64_t firstDay = daysFromCivil(from);
    const int64_t lastDay = daysFromCivil(to);
    const auto ranges = splitIntoPeriods(firstDay, lastDay, period);

    std::vector<EnergyBucket> buckets;
    buckets.reserve(ranges.size());
    for (const auto& range : ranges) {
        EnergyBucket bucket;
        bucket.start = civilFromDays(range.startDay);
        bucket.end = civilFromDays(range.endDay);
        buckets.push_back(bucket);
    }

    std::optional<int64_t> previousTotal;
    for (const auto& sample : samples) {
        const auto day = localDayOf(sample.timestamp);
        if (!day || *day > lastDay) continue;
        if (*day >= firstDay) {
            EnergyBucket& bucket = buckets[bucketIndexOf(ranges, *day)];
            ++bucket.sampleCount;
            // 뒤 기록이 속한 구간에 귀속. 줄어든 값(기기 카운터 초기화)은 0으로 본다
            if (previousTotal && sample.total_mwh > *previousTotal) {
                bucket.energyUsed_mWh += sample.total_mwh - *previousTotal;
            }
        }
        previousTotal = sample.total_mwh;
    }
    return buckets;
}
