#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include "Clock.hpp"

// 지연 명령(PC OFF 45초 등)을 기기당 최대 1개씩 들고 있다가 만기에 실행한다.
// 표는 mutex_ 안에서 메모리만 다루고, 만기 작업은 표에서 꺼낸 뒤 락 밖에서 실행한다.
struct PendingCommand {
    std::string command;
    int remainingSeconds = 0; // 올림. 만기가 지났어도 아직 실행 전이면 0
};

// "HH:MM:SS"(서버 로컬 시각). 로그용 순수 함수
std::string formatClockTime(std::chrono::system_clock::time_point time);

class DelayedCommandScheduler {
public:
    using Task = std::function<void()>;

    explicit DelayedCommandScheduler(Clock& clock);
    ~DelayedCommandScheduler();
    DelayedCommandScheduler(const DelayedCommandScheduler&) = delete;
    DelayedCommandScheduler& operator=(const DelayedCommandScheduler&) = delete;

    // 이미 그 기기의 대기 작업이 있거나 정지 뒤면 false(기존 작업은 그대로)
    bool schedule(const std::string& deviceId, const std::string& command,
                  std::chrono::milliseconds delay, Task task);
    std::optional<PendingCommand> pending(const std::string& deviceId);
    // 대기 작업이 있어 취소했으면 true. reason은 로그에만 쓴다
    bool cancel(const std::string& deviceId, const std::string& reason = "다른 명령");
    // 만기가 된 작업을 만기 순서로 실행한다. 스레드가 부르며, 테스트는 FakeClock을 움직인 뒤 직접 부른다
    void tick(std::chrono::steady_clock::time_point now);

    // 스레드는 SystemClock일 때만 의미가 있다(wait_until이 실제 steady 시계를 쓴다). FakeClock을 주입했다면 start()하지 말고 tick만 부른다
    void start();
    // 새 예약을 막고 남은 작업을 취소한 뒤, 실행 중인 작업이 끝나기를 기다려 스레드를 join한다
    void stopAndCancelAll();

private:
    struct Entry {
        std::string command;
        std::chrono::steady_clock::time_point dueAt;
        Task task;
    };

    void run();
    std::optional<std::chrono::steady_clock::time_point> earliestDueAtLocked() const;

    Clock& clock_;
    std::mutex lifecycleMutex_;        // start·stopAndCancelAll 직렬화(thread_ 생명). mutex_보다 먼저 잡는다
    std::mutex mutex_;
    std::condition_variable wakeUp_;   // 새 예약·취소·정지 때 스레드를 깨운다
    std::condition_variable idle_;     // 실행 중인 작업이 끝났을 때
    std::unordered_map<std::string, Entry> entries_;
    int runningTasks_ = 0;
    bool isStopping_ = false;
    std::thread thread_;
};
