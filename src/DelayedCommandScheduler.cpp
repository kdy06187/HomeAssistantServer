#include "DelayedCommandScheduler.hpp"
#include <ctime>
#include <iostream>

std::string formatClockTime(std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    localtime_r(&seconds, &local);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

namespace {
// 남은 시간을 초 단위로 올림한다(0.1초 남아도 1초)
int ceilSeconds(std::chrono::steady_clock::duration remaining) {
    if (remaining <= std::chrono::steady_clock::duration::zero()) return 0;
    return static_cast<int>(std::chrono::ceil<std::chrono::seconds>(remaining).count());
}
}  // namespace

DelayedCommandScheduler::DelayedCommandScheduler(Clock& clock) : clock_(clock) {}

DelayedCommandScheduler::~DelayedCommandScheduler() {
    stopAndCancelAll();
}

bool DelayedCommandScheduler::schedule(const std::string& deviceId, const std::string& command,
                                       std::chrono::milliseconds delay, Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isStopping_ || entries_.count(deviceId) > 0) return false;
        entries_[deviceId] = Entry{command, clock_.steadyNow() + delay, std::move(task)};
    }
    wakeUp_.notify_all(); // 새 작업이 더 이른 만기일 수 있다
    std::cout << "[Scheduler] 지연 " << command << " 예약: " << deviceId << ", 실행 예정 "
              << formatClockTime(clock_.utcNow() + delay) << "(" << std::chrono::ceil<std::chrono::seconds>(delay).count() << "초 뒤)" << std::endl;
    return true;
}

std::optional<PendingCommand> DelayedCommandScheduler::pending(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(deviceId);
    if (it == entries_.end()) return std::nullopt;
    return PendingCommand{it->second.command, ceilSeconds(it->second.dueAt - clock_.steadyNow())};
}

bool DelayedCommandScheduler::cancel(const std::string& deviceId, const std::string& reason) {
    Entry removed; // 락 밖에서 소멸한다: task가 잡고 있던 객체의 소멸자가 락 안에서 돌면 안 된다
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(deviceId);
        if (it == entries_.end()) return false;
        removed = std::move(it->second);
        entries_.erase(it);
    }
    wakeUp_.notify_all();
    std::cout << "[Scheduler] 지연 " << removed.command << " 취소: " << deviceId << ", "
              << formatClockTime(clock_.utcNow()) << " (사유: " << reason << ")" << std::endl;
    return true;
}

void DelayedCommandScheduler::tick(std::chrono::steady_clock::time_point now) {
    // 한 번에 하나씩 꺼낸다: 정지가 시작되면 같은 시점에 만기였던 나머지는 실행하지 않는다
    while (true) {
        std::string deviceId;
        std::string command;
        Task task;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (isStopping_) return;
            auto earliest = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->second.dueAt > now) continue;
                if (earliest == entries_.end() || it->second.dueAt < earliest->second.dueAt) earliest = it;
            }
            if (earliest == entries_.end()) return;
            deviceId = earliest->first;
            command = earliest->second.command;
            task = std::move(earliest->second.task);
            entries_.erase(earliest);
            ++runningTasks_;
        }

        std::cout << "[Scheduler] 지연 " << command << " 실행: " << deviceId << ", "
                  << formatClockTime(clock_.utcNow()) << std::endl;
        try {
            task(); // 락 밖: 작업이 chip-tool을 부르거나 다시 schedule해도 막히지 않는다
        } catch (const std::exception& e) {
            std::cerr << "[Scheduler] 지연 작업 예외: " << deviceId << " (" << e.what() << ")" << std::endl;
        } catch (...) {
            std::cerr << "[Scheduler] 지연 작업 알 수 없는 예외: " << deviceId << std::endl;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --runningTasks_;
        }
        idle_.notify_all();
    }
}

void DelayedCommandScheduler::start() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable() || isStopping_) return;
    thread_ = std::thread(&DelayedCommandScheduler::run, this);
}

void DelayedCommandScheduler::stopAndCancelAll() {
    // 동시에 두 번 불려도 둘 다 정리가 끝난 뒤에 돌아가도록, 처음부터 끝(join)까지 한 락으로 직렬화한다
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    std::unordered_map<std::string, Entry> cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isStopping_) {
            isStopping_ = true;
            cancelled.swap(entries_);
        }
    }
    for (const auto& item : cancelled) {
        std::cout << "[Scheduler] 지연 " << item.second.command << " 취소: " << item.first << ", "
                  << formatClockTime(clock_.utcNow()) << " (사유: 서버 종료)" << std::endl;
    }
    wakeUp_.notify_all();

    // tick을 스레드 밖에서 직접 부르는 경우(테스트)도 있어 실행 중 작업 수로도 기다린다
    {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return runningTasks_ == 0; });
    }
    if (thread_.joinable()) thread_.join();
}

std::optional<std::chrono::steady_clock::time_point> DelayedCommandScheduler::earliestDueAtLocked() const {
    std::optional<std::chrono::steady_clock::time_point> earliest;
    for (const auto& item : entries_) {
        if (!earliest || item.second.dueAt < *earliest) earliest = item.second.dueAt;
    }
    return earliest;
}

void DelayedCommandScheduler::run() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (isStopping_) return;
            const auto dueAt = earliestDueAtLocked();
            if (!dueAt) {
                wakeUp_.wait(lock);
            } else {
                wakeUp_.wait_until(lock, *dueAt);
            }
            if (isStopping_) return;
        }
        tick(clock_.steadyNow());
    }
}
