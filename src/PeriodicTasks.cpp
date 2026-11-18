#include "PeriodicTasks.hpp"
#include "DeviceManager.hpp"
#include <iostream>

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
    std::cout << "[DeviceManager] 🛡️ 백그라운드 헬스 체크 데몬 가동 (주기: 60초)" << std::endl;
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
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // 종료 신호로 깨어나면 true. 락은 대기 중에만 잡고 점검은 락 밖에서 한다
            if (wakeUp_.wait_for(lock, interval, [this] { return !isRunning_; })) return;
        }
        deviceManager_.runHealthCheck();
    }
}
