#pragma once
#include <chrono>
#include <optional>
#include <string>

// DB 저장 형식("YYYY-MM-DD HH:MM:SS", UTC)과 시각 사이의 변환. 같은 모양의 문자열은 사전순 비교가 시각순이다.
namespace UtcTime {
    std::string toDbText(std::chrono::system_clock::time_point time);
    // 형식이 틀리거나 달력에 없는 시각이면 nullopt
    std::optional<std::chrono::system_clock::time_point> fromDbText(const std::string& dbText);
    // "2026-11-16 00:20:00" -> "2026-11-16T00:20:00Z". 형식이 틀리면 입력 그대로
    std::string toIso8601(const std::string& dbText);
}
