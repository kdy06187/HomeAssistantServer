#pragma once
#include "Clock.hpp"
#include <mutex>

class FakeClock final : public Clock {
public:
    explicit FakeClock(std::chrono::steady_clock::time_point steadyStart = std::chrono::steady_clock::time_point{},
                       std::chrono::system_clock::time_point utcStart = std::chrono::system_clock::time_point{})
        : mSteady(steadyStart), mUtc(utcStart) {}

    // 스케줄러 스레드와 테스트 스레드가 동시에 접근할 수 있어 뮤텍스로 보호한다.
    std::chrono::steady_clock::time_point steadyNow() const override {
        std::lock_guard<std::mutex> lock(mMutex);
        return mSteady;
    }
    std::chrono::system_clock::time_point utcNow() const override {
        std::lock_guard<std::mutex> lock(mMutex);
        return mUtc;
    }
    void advance(std::chrono::nanoseconds amount) {
        std::lock_guard<std::mutex> lock(mMutex);
        mSteady += std::chrono::duration_cast<std::chrono::steady_clock::duration>(amount);
        mUtc += std::chrono::duration_cast<std::chrono::system_clock::duration>(amount);
    }

private:
    mutable std::mutex mMutex;
    std::chrono::steady_clock::time_point mSteady;
    std::chrono::system_clock::time_point mUtc;
};
