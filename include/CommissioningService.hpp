#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "Clock.hpp"
#include "Device.hpp"
#include "DeviceManager.hpp"
#include "DriverType.hpp"
#include "ProtocolDriver.hpp"
#include "Result.hpp"

enum class JobState { Pending, Running, Succeeded, Failed };

// E8 응답에 쓰는 값. 요청(비밀값)은 담지 않는다
struct JobStatus {
    std::string jobId;
    JobState state = JobState::Pending;
    std::string deviceId;                 // 성공했을 때만
    bool restored = false;
    ErrorCode errorCode = ErrorCode::None; // 실패했을 때만: CommissionFailed·CommissionTimeout·Cancelled·AlreadyRegistered
};

// 커미셔닝을 작업 스레드 하나에서 한 번에 하나씩 실행하고, 작업 상태를 메모리에만 둔다.
// 락: mutex_(작업 표·대기 작업, 메모리만). 드라이버·DeviceManager 호출은 락 밖에서 한다
class CommissioningService {
public:
    static constexpr std::chrono::hours kFinishedJobRetention{1};
    static constexpr std::size_t kMaxJobs = 64;

    // devices·드라이버·clock은 이 객체보다 오래 살아야 한다(main에서 이 객체를 나중에 선언)
    CommissioningService(DeviceManager& devices, std::unordered_map<ProtocolType, ProtocolDriver*> drivers, Clock& clock);
    ~CommissioningService();  // stop()
    CommissioningService(const CommissioningService&) = delete;
    CommissioningService& operator=(const CommissioningService&) = delete;

    // 접수하면 jobId. 진행 중(대기·실행) 작업이 있으면 CommissionBusy, 취소 요청 뒤면 Cancelled
    Result<std::string> submit(ProtocolType type, CommissionRequest request, std::vector<DriverType> driverTypes);
    // 없거나 보관 시간이 지난 작업은 JobNotFound
    Result<JobStatus> getJob(const std::string& jobId);

    void start();
    // 진행 중 커미셔닝에 취소를 알리고 새 접수를 막는다(기다리지 않음). 종료 0단계에서 부른다
    void requestCancel();
    // requestCancel + 작업 스레드 join. 여러 번 불러도 안전하다
    void stop();

    // 작업 한 건: 활성 ID 사본 → commission(취소 플래그) → 비밀값 비움 → registerCommissioned(취소 판정·확정 훅 포함).
    // 드라이버 예외는 CommissionFailed로 바꾼다(로그는 요청 비밀값을 가림). 작업 스레드가 쓰고, 시험은 동기로 직접 부른다
    static Result<RegisterOutcome> commissionOnce(DeviceManager& devices, ProtocolDriver* driver, ProtocolType type,
                                                  CommissionRequest request, const std::vector<DriverType>& driverTypes,
                                                  const std::atomic<bool>& cancelRequested);
    // E8에 내보내는 오류 코드로 좁힌다(그 밖의 코드, 예: DB 저장 실패 Internal은 CommissionFailed)
    static ErrorCode toJobErrorCode(ErrorCode code);
    // "c-<epoch초 36진>-"(재시작하면 시작 시각이 달라 옛 jobId와 겹치지 않는다)
    static std::string jobIdPrefixFor(std::chrono::system_clock::time_point startTime);

private:
    struct Job {
        JobStatus status;
        std::chrono::steady_clock::time_point finishedAt{};  // 끝난 작업만
    };
    struct PendingWork {
        std::string jobId;
        ProtocolType type = ProtocolType::UNKNOWN;
        CommissionRequest request;
        std::vector<DriverType> driverTypes;
    };

    void workerLoop();
    void finishLocked(const std::string& jobId, const Result<RegisterOutcome>& result);
    // 보관 시간이 지난 끝난 작업을 지운다
    void pruneExpiredLocked(std::chrono::steady_clock::time_point now);
    // 상한이면 가장 오래된 끝난 작업부터 지운다
    void makeRoomLocked();

    DeviceManager& devices_;
    const std::unordered_map<ProtocolType, ProtocolDriver*> drivers_;
    Clock& clock_;
    const std::string jobIdPrefix_;

    std::mutex lifecycleMutex_;  // start·stop 직렬화, 스레드 생성·join
    std::thread worker_;
    std::atomic<bool> cancelRequested_{false};

    std::mutex mutex_;
    std::condition_variable workAvailable_;
    bool stopping_ = false;                 // mutex_
    std::optional<PendingWork> pending_;    // mutex_. 작업 스레드가 꺼내면 비운다(비밀값을 오래 두지 않음)
    std::unordered_map<std::string, Job> jobs_;  // mutex_
    std::deque<std::string> jobOrder_;      // mutex_. 접수 순서(오래된 것부터)
    uint64_t nextSequence_ = 1;             // mutex_
};
