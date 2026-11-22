#include "PeriodicTasks.hpp"
#include "DeviceManager.hpp"
#include "RequestValidation.hpp"
#include <iostream>

namespace {
constexpr long long kFirstCleanupTick = 60;    // 60초 x 60 = 1시간
constexpr long long kCleanupIntervalTicks = 1440; // 60초 x 1440 = 24시간
}

bool shouldRunCleanup(long long tickCount) {
    if (tickCount < kFirstCleanupTick) return false;
    return (tickCount - kFirstCleanupTick) % kCleanupIntervalTicks == 0;
}

PeriodicTasks::PeriodicTasks(DeviceManager& deviceManager) : deviceManager_(deviceManager) {}

PeriodicTasks::~PeriodicTasks() {
    stop();
}

void PeriodicTasks::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isRunning_) return;
        isRunning_ = true;
    }
    thread_ = std::thread(&PeriodicTasks::run, this);
    std::cout << "[DeviceManager] 🛡️ 백그라운드 헬스 체크 데몬 가동 (주기: 60초, 기록 정리: 1시간 뒤부터 24시간마다)" << std::endl;
}

void PeriodicTasks::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isRunning_) return;
        isRunning_ = false;
    }
    wakeUp_.notify_all(); // 대기 중인 스레드를 바로 깨운다
    if (thread_.joinable()) {
        thread_.join();
    }
    std::cout << "[DeviceManager] 🛡️ 헬스 체크 데몬 종료" << std::endl;
}

void PeriodicTasks::run() {
    const auto interval = std::chrono::seconds(60);
    long long tickCount = 0;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // 종료 신호로 깨어나면 true. 락은 대기 중에만 잡고 점검은 락 밖에서 한다
            if (wakeUp_.wait_for(lock, interval, [this] { return !isRunning_; })) return;
        }
        // 예외가 스레드 밖으로 나가면 서버가 종료되므로 로그만 남기고 다음 주기로 넘어간다
        try {
            deviceManager_.runHealthCheck();
        } catch (const std::exception& e) {
            std::cerr << "[PeriodicTasks] 경고 : 헬스체크 예외, 다음 주기에 다시 시도 (" << RequestValidation::sanitizeForLog(e.what()) << ")" << std::endl;
        }
        ++tickCount;
        // 정리 중에 종료 신호가 와도 트랜잭션이 끝난 뒤 stop()의 join이 풀린다
        if (shouldRunCleanup(tickCount)) {
            try {
                deviceManager_.runRetentionCleanup();
            } catch (const std::exception& e) {
                std::cerr << "[PeriodicTasks] 경고 : 기록 정리 예외, 다음 주기에 다시 시도 (" << RequestValidation::sanitizeForLog(e.what()) << ")" << std::endl;
            }
        }
    }
}
