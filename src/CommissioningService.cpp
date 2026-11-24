#include "CommissioningService.hpp"
#include <algorithm>
#include <iostream>
#include <utility>
#include "ProcessChipToolRunner.hpp"
#include "RequestValidation.hpp"

CommissioningService::CommissioningService(DeviceManager& devices,
                                           std::unordered_map<ProtocolType, ProtocolDriver*> drivers, Clock& clock)
    : devices_(devices), drivers_(std::move(drivers)), clock_(clock), jobIdPrefix_(jobIdPrefixFor(clock.utcNow())) {}

CommissioningService::~CommissioningService() { stop(); }

std::string CommissioningService::jobIdPrefixFor(std::chrono::system_clock::time_point startTime) {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(startTime.time_since_epoch()).count();
    if (seconds < 0) seconds = 0;
    std::string base36;
    do {
        base36.insert(base36.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[seconds % 36]);
        seconds /= 36;
    } while (seconds > 0);
    return "c-" + base36 + "-";
}

ErrorCode CommissioningService::toJobErrorCode(ErrorCode code) {
    switch (code) {
    case ErrorCode::CommissionTimeout:
    case ErrorCode::Cancelled:
    case ErrorCode::AlreadyRegistered:
        return code;
    default:
        return ErrorCode::CommissionFailed;
    }
}

namespace {
// 예외 메시지에 요청 값이 섞여 나올 수 있어 로그 전에 가린다(SSID는 짧으면 일반 단어와 겹치므로 4바이트 이상만)
std::string maskRequestSecrets(const std::string& text, const CommissionRequest& request) {
    std::vector<std::string> secrets;
    if (!request.password.empty()) secrets.push_back(request.password);
    if (!request.payload.empty()) secrets.push_back(request.payload);
    if (request.ssid.size() >= 4) secrets.push_back(request.ssid);
    return RequestValidation::sanitizeForLog(ProcessChipToolRunner::maskSecrets(text, secrets));
}
}  // namespace

Result<RegisterOutcome> CommissioningService::commissionOnce(DeviceManager& devices, ProtocolDriver* driver, ProtocolType type,
                                                             CommissionRequest request,
                                                             const std::vector<DriverType>& driverTypes,
                                                             const std::atomic<bool>& cancelRequested) {
    if (!driver) return Result<RegisterOutcome>::failure(ErrorCode::Internal, "protocol driver not found");
    if (cancelRequested) return Result<RegisterOutcome>::failure(ErrorCode::Cancelled, "cancelled before start");

    Result<CommissionOutcome> outcome;
    try {
        // 드라이버에는 활성 ID 사본만 넘긴다(수십 초 동안 DeviceManager 락을 쥐지 않음)
        outcome = driver->commission(request, devices.activeDeviceIds(), cancelRequested);
    } catch (const std::exception& e) {
        // 작업 스레드에서 예외가 빠져나가면 std::terminate로 서버가 죽는다
        std::cerr << "[CommissioningService] 커미셔닝 중 예외 : " << maskRequestSecrets(e.what(), request) << std::endl;
        outcome = Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "driver exception");
    } catch (...) {
        std::cerr << "[CommissioningService] 커미셔닝 중 알 수 없는 예외" << std::endl;
        outcome = Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "driver exception");
    }
    // 등록 단계(같은 ID 삭제를 최대 10초 기다릴 수 있음)까지 비밀값을 들고 가지 않는다. 이름만 남긴다
    request.payload.clear();
    request.ssid.clear();
    request.password.clear();
    if (!outcome.ok()) return Result<RegisterOutcome>::failure(outcome.error.code, outcome.error.message);
    // 드라이버 성공 직후 취소됐는지는 registerCommissioned가 ID 락 안에서 판정하고 드라이버 정리까지 한다
    return devices.registerCommissioned(type, *outcome.value, request.name, driverTypes, cancelRequested);
}

Result<std::string> CommissioningService::submit(ProtocolType type, CommissionRequest request,
                                                 std::vector<DriverType> driverTypes) {
    if (cancelRequested_) return Result<std::string>::failure(ErrorCode::Cancelled, "shutting down");
    std::string jobId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // requestCancel도 이 락 안에서 플래그를 세우므로, 여기서 다시 보면 "취소 뒤 접수"가 생기지 않는다
        if (stopping_ || cancelRequested_) return Result<std::string>::failure(ErrorCode::Cancelled, "shutting down");
        for (const auto& entry : jobs_) {
            const JobState state = entry.second.status.state;
            if (state == JobState::Pending || state == JobState::Running) {
                return Result<std::string>::failure(ErrorCode::CommissionBusy, "another commissioning in progress");
            }
        }
        pruneExpiredLocked(clock_.steadyNow());
        makeRoomLocked();
        jobId = jobIdPrefix_ + std::to_string(nextSequence_++);
        Job job;
        job.status.jobId = jobId;
        jobs_.emplace(jobId, std::move(job));
        jobOrder_.push_back(jobId);
        pending_ = PendingWork{jobId, type, std::move(request), std::move(driverTypes)};
    }
    workAvailable_.notify_one();
    std::cout << "[CommissioningService] 등록 작업 접수 : " << jobId << " (" << devices_.getProtocolString(type) << ")" << std::endl;
    return Result<std::string>::success(jobId);
}

Result<JobStatus> CommissioningService::getJob(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneExpiredLocked(clock_.steadyNow());
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return Result<JobStatus>::failure(ErrorCode::JobNotFound, "job not found");
    return Result<JobStatus>::success(it->second.status);
}

void CommissioningService::start() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    if (worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;  // 멈춘 서비스는 다시 시작하지 않는다
    }
    worker_ = std::thread([this] { workerLoop(); });
}

void CommissioningService::requestCancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelRequested_ = true;
}

void CommissioningService::stop() {
    requestCancel();
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        // 아직 시작하지 않은 작업은 실행하지 않고 취소로 끝낸다
        if (pending_) {
            finishLocked(pending_->jobId, Result<RegisterOutcome>::failure(ErrorCode::Cancelled, "stopped before start"));
            pending_.reset();
        }
    }
    workAvailable_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void CommissioningService::workerLoop() {
    while (true) {
        PendingWork work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workAvailable_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            work = std::move(*pending_);
            pending_.reset();
            jobs_[work.jobId].status.state = JobState::Running;
        }
        ProtocolDriver* driver = nullptr;
        const auto driverIt = drivers_.find(work.type);
        if (driverIt != drivers_.end()) driver = driverIt->second;

        // 요청(비밀값)은 commissionOnce로 넘겨 그 안에서 commission 직후 비운다(작업 표에는 처음부터 없다)
        const auto result = commissionOnce(devices_, driver, work.type, std::move(work.request), work.driverTypes, cancelRequested_);
        // 결과 로그를 먼저 남긴다: E8이 끝남을 본 뒤에는 이 작업이 더 쓰는 것이 없다
        if (result.ok()) {
            std::cout << "[CommissioningService] 등록 작업 성공 : " << work.jobId << " ("
                      << RequestValidation::sanitizeForLog(result.value->deviceId) << (result.value->restored ? ", 기록 복구" : "")
                      << ")" << std::endl;
        } else {
            std::cerr << "[CommissioningService] 등록 작업 실패 : " << work.jobId << " ("
                      << RequestValidation::sanitizeForLog(result.error.message) << ")" << std::endl;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        finishLocked(work.jobId, result);
    }
}

void CommissioningService::finishLocked(const std::string& jobId, const Result<RegisterOutcome>& result) {
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;
    JobStatus& status = it->second.status;
    if (result.ok()) {
        status.state = JobState::Succeeded;
        status.deviceId = result.value->deviceId;
        status.restored = result.value->restored;
    } else {
        status.state = JobState::Failed;
        status.errorCode = toJobErrorCode(result.error.code);
    }
    it->second.finishedAt = clock_.steadyNow();
}

void CommissioningService::pruneExpiredLocked(std::chrono::steady_clock::time_point now) {
    for (auto orderIt = jobOrder_.begin(); orderIt != jobOrder_.end();) {
        const auto jobIt = jobs_.find(*orderIt);
        const bool finished = jobIt != jobs_.end() && (jobIt->second.status.state == JobState::Succeeded ||
                                                       jobIt->second.status.state == JobState::Failed);
        if (jobIt == jobs_.end() || (finished && now - jobIt->second.finishedAt >= kFinishedJobRetention)) {
            if (jobIt != jobs_.end()) jobs_.erase(jobIt);
            orderIt = jobOrder_.erase(orderIt);
        } else {
            ++orderIt;
        }
    }
}

void CommissioningService::makeRoomLocked() {
    // 진행 중 작업은 submit이 이미 막았으므로 남은 것은 모두 끝난 작업이다
    while (jobs_.size() >= kMaxJobs && !jobOrder_.empty()) {
        jobs_.erase(jobOrder_.front());
        jobOrder_.pop_front();
    }
}
