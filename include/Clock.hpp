#pragma once
#include <chrono>

// 시각을 직접 읽지 않고 주입받아 쓰면 테스트에서 시간을 마음대로 움직일 수 있다.
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::steady_clock::time_point steadyNow() const = 0; // 경과 시간 측정용
    virtual std::chrono::system_clock::time_point utcNow() const = 0;    // 기록·표시용 실제 시각
};

class SystemClock final : public Clock {
public:
    std::chrono::steady_clock::time_point steadyNow() const override { return std::chrono::steady_clock::now(); }
    std::chrono::system_clock::time_point utcNow() const override { return std::chrono::system_clock::now(); }
};
