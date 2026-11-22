#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "DatabaseManager.hpp"
#include "RequestValidation.hpp"

// 기록(UTC)을 한국 표준시(+09:00 고정) 날짜 구간으로 모아 사용량을 계산하는 순수 함수 모음.
// 날짜 계산은 표준 라이브러리에 달력이 없어 직접 구현한다.

struct EnergyBucket {
    CivilDate start;           // 구간 첫날(포함)
    CivilDate end;             // 구간 끝날(포함)
    int64_t energyUsed_mWh = 0;
    int64_t sampleCount = 0;
};

// 조회 범위를 DB 시각(UTC) 반개구간 [fromUtc, toUtc)으로 바꾼 값
struct UtcRange {
    std::string fromUtc;
    std::string toUtc;
};

namespace EnergyAggregation {
    constexpr int kUtcOffsetHours = 9;

    // 1970-01-01부터의 일 수 <-> 날짜 (Howard Hinnant의 공개 알고리즘)
    int64_t daysFromCivil(const CivilDate& date);
    CivilDate civilFromDays(int64_t days);

    std::string formatDate(const CivilDate& date); // YYYY-MM-DD

    // from 00:00+09:00 ~ (to+1일) 00:00+09:00
    UtcRange localDateRangeToUtc(const CivilDate& from, const CivilDate& to);

    // samples는 오래된 순이며, 범위 시작 직전 기록 1건이 앞에 있어도 된다(첫 구간 사용량 계산용).
    // 기록이 없는 구간도 sampleCount 0으로 포함한다
    std::vector<EnergyBucket> aggregate(const std::vector<EnergyLog>& samples, const CivilDate& from,
                                        const CivilDate& to, HistoryPeriod period);
}
