#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

class DeviceManager;

// 60초마다 헬스체크를 돌리는 백그라운드 스레드. 첫 점검은 start 60초 뒤.
class PeriodicTasks {
public:
    explicit PeriodicTasks(DeviceManager& deviceManager);
    ~PeriodicTasks();
    PeriodicTasks(const PeriodicTasks&) = delete;
    PeriodicTasks& operator=(const PeriodicTasks&) = delete;

    void start();
    void stop();

private:
    void run();

    DeviceManager& deviceManager_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wakeUp_;
    bool isRunning_ = false;
};
