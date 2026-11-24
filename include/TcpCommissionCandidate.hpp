#pragma once
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

// TCP 커미셔닝에서 "방금 Wi-Fi 설정을 받고 접속한 기기"를 고르는 순수 함수
namespace TcpCommissionCandidate {

// 아래를 모두 만족하는 ID 중 사전순 첫 번째(여러 개면 결과가 실행마다 같도록):
// 커미셔닝 시작 전 연결 목록(before)에 없음, MAC으로 식별된 ID(TCP_ 접두사), 이미 등록된 기기 아님, PC 에이전트 아님.
// 미식별 연결은 연결 목록에 없으므로 처음부터 후보가 아니다
std::optional<std::string> pickNewDevice(const std::vector<std::string>& before, const std::vector<std::string>& now,
                                         const std::unordered_set<std::string>& activeIds,
                                         const std::optional<std::string>& pcAgentId);

// 커미셔닝 시작 전 연결 목록(before)에 없던 활성(이미 등록된) TCP_ ID 중 사전순 첫 번째.
// 새 후보 없이 대기가 끝났을 때 이것이 있으면 "등록된 기기를 초기화해 다시 등록하려 함"으로 본다
std::optional<std::string> findReconnectedActive(const std::vector<std::string>& before, const std::vector<std::string>& now,
                                                 const std::unordered_set<std::string>& activeIds);

}  // namespace TcpCommissionCandidate
