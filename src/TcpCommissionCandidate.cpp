#include "TcpCommissionCandidate.hpp"
#include <algorithm>

std::optional<std::string> TcpCommissionCandidate::pickNewDevice(const std::vector<std::string>& before,
                                                                 const std::vector<std::string>& now,
                                                                 const std::unordered_set<std::string>& activeIds,
                                                                 const std::optional<std::string>& pcAgentId) {
    const std::unordered_set<std::string> existing(before.begin(), before.end());
    std::vector<std::string> sorted = now;
    std::sort(sorted.begin(), sorted.end());
    for (const auto& id : sorted) {
        if (existing.count(id)) continue;
        if (id.rfind("TCP_", 0) != 0) continue;
        if (activeIds.count(id)) continue;  // 재접속한 기존 기기
        if (pcAgentId && id == *pcAgentId) continue;
        return id;
    }
    return std::nullopt;
}
