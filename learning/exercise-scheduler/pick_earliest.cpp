// 빈칸 과제 2: 만기가 된 작업 중 가장 이른 것의 id를 고른다(tick 안쪽 반복문의 축소판).
// 빌드: g++ -std=c++17 pick_earliest.cpp -o pick_earliest && ./pick_earliest
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
using Time = long long; // 테스트 편의를 위해 정수 "초"로 대신한다

// entries: id -> dueAt. now 이하(만기 지남)인 것 중 dueAt이 가장 작은 id. 없으면 nullopt.
std::optional<std::string> pickEarliestDue(const std::unordered_map<std::string, Time>& entries, Time now) {
    // TODO
    return std::nullopt;
}

int main() {
    std::unordered_map<std::string, Time> e{{"a", 50}, {"b", 20}, {"c", 90}};
    int fails = 0;
    auto expect = [&](Time now, std::optional<std::string> want) {
        if (pickEarliestDue(e, now) != want) { ++fails; std::cout << "FAIL now=" << now << "\n"; }
    };
    expect(10, std::nullopt);
    expect(20, "b");
    expect(60, "b");
    e.erase("b");
    expect(60, "a");
    std::cout << (fails ? "일부 실패\n" : "모두 통과\n");
    return fails;
}
