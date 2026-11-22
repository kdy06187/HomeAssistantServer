#include "UtcTime.hpp"
#include <cstdio>
#include <ctime>

namespace {

constexpr size_t kDbTextLength = 19;

bool hasDbTextShape(const std::string& text) {
    if (text.size() != kDbTextLength) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const bool isSeparator = (i == 4 || i == 7 || i == 10 || i == 13 || i == 16);
        const char expected = (i == 10) ? ' ' : (i == 13 || i == 16) ? ':' : '-';
        if (isSeparator ? c != expected : (c < '0' || c > '9')) return false;
    }
    return true;
}

}  // namespace

std::string UtcTime::toDbText(std::chrono::system_clock::time_point time) {
    // 초 미만은 버린다(음수 시각도 내림)
    const auto seconds = std::chrono::floor<std::chrono::seconds>(time).time_since_epoch().count();
    const std::time_t rawTime = static_cast<std::time_t>(seconds);
    std::tm utc{};
    gmtime_r(&rawTime, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &utc);
    return buffer;
}

std::optional<std::chrono::system_clock::time_point> UtcTime::fromDbText(const std::string& dbText) {
    if (!hasDbTextShape(dbText)) return std::nullopt;
    std::tm parsed{};
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(dbText.c_str(), "%4d-%2d-%2d %2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return std::nullopt;
    }
    parsed.tm_year = year - 1900;
    parsed.tm_mon = month - 1;
    parsed.tm_mday = day;
    parsed.tm_hour = hour;
    parsed.tm_min = minute;
    parsed.tm_sec = second;
    const std::time_t rawTime = timegm(&parsed); // 범위를 넘는 값(02-30 등)은 다음 달로 넘어가 버린다
    std::tm roundTrip{};
    gmtime_r(&rawTime, &roundTrip);
    if (roundTrip.tm_year != year - 1900 || roundTrip.tm_mon != month - 1 || roundTrip.tm_mday != day ||
        roundTrip.tm_hour != hour || roundTrip.tm_min != minute || roundTrip.tm_sec != second) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(rawTime);
}

std::string UtcTime::toIso8601(const std::string& dbText) {
    if (!hasDbTextShape(dbText)) return dbText;
    std::string iso = dbText;
    iso[10] = 'T';
    return iso + "Z";
}
