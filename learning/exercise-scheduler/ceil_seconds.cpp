// 빈칸 과제: 남은 시간을 초 단위로 "올림"한다. 원본 src/는 건드리지 않는 독립 사본.
// 빌드: g++ -std=c++17 ceil_seconds.cpp -o ceil_seconds && ./ceil_seconds
#include <chrono>
#include <iostream>
using Duration = std::chrono::steady_clock::duration;

// 규칙: remaining <= 0 이면 0, 0.1초 남아도 1, 1.0초는 1, 1.001초는 2.
// 힌트: std::chrono::ceil<std::chrono::seconds>(...) 는 올림, duration_cast 는 버림.
int ceilSeconds(Duration remaining) {
    // TODO
    return -1;
}

int main() {
    using namespace std::chrono;
    int fails = 0;
    auto check = [&](Duration d, int want) {
        int got = ceilSeconds(d);
        if (got != want) { ++fails; std::cout << "FAIL want " << want << " got " << got << "\n"; }
    };
    check(milliseconds(-5), 0);
    check(milliseconds(0), 0);
    check(milliseconds(100), 1);
    check(milliseconds(1000), 1);
    check(milliseconds(1001), 2);
    check(seconds(35), 35);
    std::cout << (fails ? "일부 실패\n" : "모두 통과\n");
    return fails;
}
