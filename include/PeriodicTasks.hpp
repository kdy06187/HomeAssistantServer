#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

class DeviceManager;

// 60초 간격의 몇 번째 점검(1부터)에서 오래된 기록 정리를 할지: 60번째(시작 1시간 뒤)가 처음, 이후 1440번(24시간)마다
bool shouldRunCleanup(long long tickCount);

// 60초마다 헬스체크를 돌리고 가끔 오래된 기록 정리도 하는 백그라운드 스레드. 첫 점검은 start 60초 뒤.
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
