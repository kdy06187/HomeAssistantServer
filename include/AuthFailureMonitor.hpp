#pragma once
#include <chrono>
#include <deque>
#include <mutex>
#include "Clock.hpp"

// 인증 실패가 짧은 시간에 몰리는지 센다(차단은 하지 않고 경고 시점만 알려 준다).
// 최근 window 안의 실패가 threshold를 넘는 순간 한 번 true를 돌려주고,
// 실패가 threshold 이하로 줄어든 뒤 다시 넘을 때까지 true를 돌려주지 않는다. 락 안에서는 메모리만 쓴다.
class AuthFailureMonitor {
public:
    static constexpr std::chrono::seconds kWindow{60};
    static constexpr size_t kThreshold = 10;

    explicit AuthFailureMonitor(const Clock& clock) : mClock(clock) {}

    // 실패 한 건을 기록한다. 경고를 내야 하는 순간이면 true
    bool recordFailure() {
        std::lock_guard<std::mutex> lock(mMutex);
        // 시각을 락 안에서 읽어야 저장 순서가 시간 순서와 같다
        const auto now = mClock.steadyNow();
        mFailureTimes.push_back(now);
        // 임계+1건만 있으면 "창 안에 임계 초과" 판정이 가능하다(공격 중에도 메모리 상한)
        if (mFailureTimes.size() > kThreshold + 1) mFailureTimes.pop_front();
        dropOlderThanWindow(now);
        if (mFailureTimes.size() <= kThreshold) {
            mWarned = false;
            return false;
        }
        if (mWarned) return false;
        mWarned = true;
        return true;
    }

private:
    void dropOlderThanWindow(std::chrono::steady_clock::time_point now) {
        while (!mFailureTimes.empty() && now - mFailureTimes.front() >= kWindow) mFailureTimes.pop_front();
    }

    const Clock& mClock;
    std::mutex mMutex;
    std::deque<std::chrono::steady_clock::time_point> mFailureTimes;
    bool mWarned = false;
};
