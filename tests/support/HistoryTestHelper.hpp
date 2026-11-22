#pragma once
#include <string>
#include <vector>
#include "DatabaseManager.hpp"

// 시험에서 "범위 없이 최신 limit건"을 간단히 읽는 도우미(오래된 것부터)
inline Result<std::vector<EnergyLog>> latestLogs(DatabaseManager& database, const std::string& deviceId, int limit = 50) {
    auto page = database.queryHistory(deviceId, std::nullopt, std::nullopt, limit);
    if (!page.ok()) return Result<std::vector<EnergyLog>>::failure(page.error.code, page.error.message);
    return Result<std::vector<EnergyLog>>::success(std::move(page.value->logs));
}
