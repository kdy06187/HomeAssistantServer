#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

// DB 저장 형식("YYYY-MM-DD HH:MM:SS", UTC)과 시각 사이의 변환. 같은 모양의 문자열은 사전순 비교가 시각순이다.
namespace UtcTime {
    std::string toDbText(std::chrono::system_clock::time_point time);
    // time_point(나노초 int64, 약 1677~2262년)를 거치지 않는 변환. 먼 날짜도 안전하다
    std::string toDbTextFromEpochSeconds(int64_t epochSeconds);
    // 허용 연도 범위. 밖이면 아래 두 변환이 실패한다(time_point 오버플로 방지)
    constexpr int kMinDbYear = 1900;
    constexpr int kMaxDbYear = 2200;
    // 형식이 틀리거나 달력에 없거나 연도가 범위 밖이면 nullopt
    std::optional<int64_t> toEpochSeconds(const std::string& dbText);
    std::optional<std::chrono::system_clock::time_point> fromDbText(const std::string& dbText);
    // "2026-11-16 00:20:00" -> "2026-11-16T00:20:00Z". 형식이 틀리면 입력 그대로
    std::string toIso8601(const std::string& dbText);
}
