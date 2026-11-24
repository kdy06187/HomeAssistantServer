#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "CommissioningService.hpp"
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "UtcTime.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "support/LogCapture.hpp"

using namespace std::chrono_literals;

namespace {

const std::chrono::system_clock::time_point kStart = *UtcTime::fromDbText("2026-11-24 00:00:00");

// 한 번 열리면 계속 열려 있는 문
class Gate {
public:
    void open() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            opened_ = true;
        }
        cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return opened_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool opened_ = false;
};

class CommissioningServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::MATTER, &matterFake);
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    }
    void TearDown() override { service.stop(); }

    Result<std::string> submitPlug(const std::string& password = "pw") {
        return service.submit(ProtocolType::MATTER, CommissionRequest{"plug", "34970112332", "ssid", password}, {});
    }
    // 작업이 끝날 때까지(최대 5초) 기다린다
    JobStatus waitFinished(const std::string& jobId) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto job = service.getJob(jobId);
            if (job.ok() && (job.value->state == JobState::Succeeded || job.value->state == JobState::Failed)) return *job.value;
            std::this_thread::sleep_for(5ms);
        }
        ADD_FAILURE() << "작업이 끝나지 않음: " << jobId;
        return JobStatus{};
    }
    JobState stateOf(const std::string& jobId) { return service.getJob(jobId).value->state; }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock{{}, kStart};
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver matterFake;
    FakeProtocolDriver tcpFake;
    CommissioningService service{manager, {{ProtocolType::MATTER, &matterFake}, {ProtocolType::TCP_DIY, &tcpFake}}, clock};
};

}  // namespace

TEST_F(CommissioningServiceTest, JobGoesPendingRunningSucceeded) {
    Gate entered;
    Gate release;
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"SN-NEW"});
    matterFake.onCommission = [&] {
        entered.open();
        release.wait();
    };
    const auto jobId = submitPlug();
    ASSERT_TRUE(jobId.ok());
    EXPECT_EQ(stateOf(*jobId.value), JobState::Pending);  // 작업 스레드를 아직 시작하지 않았다

    service.start();
    entered.wait();
    EXPECT_EQ(stateOf(*jobId.value), JobState::Running);
    release.open();
    const JobStatus finished = waitFinished(*jobId.value);

    EXPECT_EQ(finished.state, JobState::Succeeded);
    EXPECT_EQ(finished.deviceId, "SN-NEW");
    EXPECT_FALSE(finished.restored);
    EXPECT_EQ(finished.errorCode, ErrorCode::None);
    EXPECT_EQ(manager.activeDeviceIds().count("SN-NEW"), 1u);
}

TEST_F(CommissioningServiceTest, SecondSubmitWhileInProgressIsBusyAndAllowedAfterFinish) {
    const auto first = submitPlug();
    ASSERT_TRUE(first.ok());
    const auto busy = service.submit(ProtocolType::TCP_DIY, CommissionRequest{"lamp", "", "", ""}, {});
    EXPECT_EQ(busy.error.code, ErrorCode::CommissionBusy);

    service.start();
    waitFinished(*first.value);
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"SN-2"});
    const auto second = submitPlug();
    ASSERT_TRUE(second.ok());
    EXPECT_NE(*first.value, *second.value);
    EXPECT_EQ(waitFinished(*second.value).state, JobState::Succeeded);
}

TEST_F(CommissioningServiceTest, DriverFailureCodesAreKeptAndOthersBecomeCommissionFailed) {
    service.start();
    const std::vector<std::pair<ErrorCode, ErrorCode>> cases = {
        {ErrorCode::CommissionFailed, ErrorCode::CommissionFailed},
        {ErrorCode::CommissionTimeout, ErrorCode::CommissionTimeout},
        {ErrorCode::Cancelled, ErrorCode::Cancelled},
        {ErrorCode::AlreadyRegistered, ErrorCode::AlreadyRegistered},
        {ErrorCode::Internal, ErrorCode::CommissionFailed},
        {ErrorCode::DeviceUnreachable, ErrorCode::CommissionFailed},
    };
    for (const auto& [driverCode, expected] : cases) {
        matterFake.commissionResult = Result<CommissionOutcome>::failure(driverCode, "x");
        const auto jobId = submitPlug();
        ASSERT_TRUE(jobId.ok());
        const JobStatus finished = waitFinished(*jobId.value);
        EXPECT_EQ(finished.state, JobState::Failed);
        EXPECT_EQ(finished.errorCode, expected);
        EXPECT_TRUE(finished.deviceId.empty());
    }
}

// 활성 기기와 같은 ID가 나오면 ALREADY_REGISTERED, DB 저장 실패는 COMMISSION_FAILED
TEST_F(CommissioningServiceTest, RegistrationOutcomesAreReported) {
    ASSERT_TRUE(manager.addDevice("SN-1", "plug", ProtocolType::MATTER, {}));
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"SN-1"});
    service.start();

    const auto already = submitPlug();
    EXPECT_EQ(waitFinished(*already.value).errorCode, ErrorCode::AlreadyRegistered);

    ASSERT_TRUE(manager.removeDevice("SN-1").ok());
    const auto restored = submitPlug();
    const JobStatus restoredJob = waitFinished(*restored.value);
    EXPECT_EQ(restoredJob.state, JobState::Succeeded);
    EXPECT_TRUE(restoredJob.restored);

    database.close();
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"SN-9"});
    const auto dbFailure = submitPlug();
    EXPECT_EQ(waitFinished(*dbFailure.value).errorCode, ErrorCode::CommissionFailed);
}

TEST_F(CommissioningServiceTest, DriverExceptionBecomesFailedInsteadOfTerminating) {
    matterFake.onCommission = [] { throw std::runtime_error("boom"); };
    service.start();
    const auto jobId = submitPlug();
    const JobStatus finished = waitFinished(*jobId.value);
    EXPECT_EQ(finished.state, JobState::Failed);
    EXPECT_EQ(finished.errorCode, ErrorCode::CommissionFailed);
}

// 작업 상태에는 요청(비밀값)이 없다. 드라이버는 원래 요청을 받는다
TEST_F(CommissioningServiceTest, JobStatusCarriesNoRequestSecrets) {
    service.start();
    const auto jobId = submitPlug("super-secret-pw");
    const JobStatus finished = waitFinished(*jobId.value);
    ASSERT_EQ(matterFake.commissionRequests.size(), 1u);
    EXPECT_EQ(matterFake.commissionRequests[0].password, "super-secret-pw");
    EXPECT_EQ(finished.jobId.find("super-secret-pw"), std::string::npos);
    EXPECT_EQ(finished.deviceId.find("super-secret-pw"), std::string::npos);
}

TEST_F(CommissioningServiceTest, FinishedJobIsKeptOneHourThenNotFound) {
    service.start();
    const auto jobId = submitPlug();
    waitFinished(*jobId.value);

    clock.advance(CommissioningService::kFinishedJobRetention - 1s);
    EXPECT_TRUE(service.getJob(*jobId.value).ok());
    clock.advance(1s);
    EXPECT_EQ(service.getJob(*jobId.value).error.code, ErrorCode::JobNotFound);
    EXPECT_EQ(service.getJob("c-nope-1").error.code, ErrorCode::JobNotFound);
}

TEST_F(CommissioningServiceTest, JobTableKeepsAtMostLimitDroppingOldestFinished) {
    service.start();
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < CommissioningService::kMaxJobs + 1; ++i) {
        matterFake.commissionResult = Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "x");
        const auto jobId = submitPlug();
        ASSERT_TRUE(jobId.ok());
        waitFinished(*jobId.value);
        ids.push_back(*jobId.value);
    }
    EXPECT_EQ(service.getJob(ids.front()).error.code, ErrorCode::JobNotFound);
    EXPECT_TRUE(service.getJob(ids[1]).ok());
    EXPECT_TRUE(service.getJob(ids.back()).ok());
}

// 종료: 진행 중 작업은 취소 플래그를 보고 Cancelled로 끝나고 stop은 1초 안에 돌아온다
TEST_F(CommissioningServiceTest, StopCancelsRunningJobAndJoinsQuickly) {
    Gate entered;
    matterFake.waitForCancel = true;
    matterFake.onCommission = [&] { entered.open(); };
    service.start();
    const auto jobId = submitPlug();
    entered.wait();

    const auto before = std::chrono::steady_clock::now();
    service.stop();
    const auto elapsed = std::chrono::steady_clock::now() - before;

    EXPECT_LT(elapsed, 1s);
    const auto job = service.getJob(*jobId.value);
    ASSERT_TRUE(job.ok());
    EXPECT_EQ(job.value->state, JobState::Failed);
    EXPECT_EQ(job.value->errorCode, ErrorCode::Cancelled);
    EXPECT_EQ(service.submit(ProtocolType::MATTER, CommissionRequest{}, {}).error.code, ErrorCode::Cancelled);
}

// 작업 스레드를 시작하기 전에 멈추면 대기 작업은 실행되지 않고 취소된다
TEST_F(CommissioningServiceTest, StopBeforeStartCancelsPendingJobWithoutCallingDriver) {
    const auto jobId = submitPlug();
    service.stop();
    EXPECT_EQ(service.getJob(*jobId.value).value->errorCode, ErrorCode::Cancelled);
    EXPECT_TRUE(matterFake.commissionRequests.empty());
    service.start();  // 멈춘 뒤에는 다시 시작하지 않는다
    EXPECT_EQ(submitPlug().error.code, ErrorCode::Cancelled);
}

// 드라이버가 성공한 직후 취소되면 등록하지 않고 드라이버에 정리를 알린다
TEST_F(CommissioningServiceTest, CancelRightAfterDriverSuccessDoesNotRegister) {
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"SN-NEW"});
    matterFake.ignoreCancel = true;
    matterFake.onCommission = [this] { service.requestCancel(); };
    service.start();
    const auto jobId = submitPlug();

    EXPECT_EQ(waitFinished(*jobId.value).errorCode, ErrorCode::Cancelled);
    EXPECT_TRUE(manager.activeDeviceIds().empty());
    std::lock_guard<std::mutex> lock(matterFake.recordMutex);
    EXPECT_EQ(matterFake.finalizedOutcomes, (std::vector<std::pair<std::string, bool>>{{"SN-NEW", false}}));
}

TEST_F(CommissioningServiceTest, MissingDriverFailsAsCommissionFailed) {
    CommissioningService bare{manager, {}, clock};
    bare.start();
    const auto jobId = bare.submit(ProtocolType::MATTER, CommissionRequest{"plug", "", "", ""}, {});
    ASSERT_TRUE(jobId.ok());
    JobStatus finished;
    for (int i = 0; i < 1000; ++i) {
        finished = *bare.getJob(*jobId.value).value;
        if (finished.state == JobState::Failed) break;
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(finished.errorCode, ErrorCode::CommissionFailed);
    bare.stop();
}

// jobId는 시작 시각(36진) + 순번이라 재시작하면(시작 시각이 다르면) 겹치지 않는다
TEST_F(CommissioningServiceTest, JobIdsDifferAcrossRestartsAndMatchPathFormat) {
    const auto first = submitPlug();
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value->rfind(CommissioningService::jobIdPrefixFor(kStart), 0), 0u);
    EXPECT_NE(CommissioningService::jobIdPrefixFor(kStart), CommissioningService::jobIdPrefixFor(kStart + 1s));
    EXPECT_EQ(CommissioningService::jobIdPrefixFor(std::chrono::system_clock::time_point{}), "c-0-");
    for (char c : *first.value) EXPECT_TRUE((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') << *first.value;
    EXPECT_LE(first.value->size(), 32u);
}

// TSan: 접수·조회·정지가 동시에 일어나도 경합 없이 끝난다
TEST_F(CommissioningServiceTest, ConcurrentSubmitGetJobAndStopAreRaceFree) {
    service.start();
    std::atomic<bool> done{false};
    std::thread submitter([&] {
        for (int i = 0; i < 200 && !done; ++i) {
            const auto jobId = submitPlug();
            if (jobId.ok()) service.getJob(*jobId.value);
        }
    });
    std::thread reader([&] {
        for (int i = 0; i < 200; ++i) service.getJob("c-x-1");
    });
    std::this_thread::sleep_for(5ms);
    service.stop();
    done = true;
    submitter.join();
    reader.join();
}

// commissionOnce: 드라이버에 활성 ID 사본과 요청을 넘긴다
TEST_F(CommissioningServiceTest, CommissionOncePassesActiveIdSnapshot) {
    ASSERT_TRUE(manager.addDevice("TCP_000000000001", "lamp", ProtocolType::TCP_DIY, {}));
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"TCP_000000000002", "192.168.0.9"});
    const std::atomic<bool> notCancelled{false};

    const auto result = CommissioningService::commissionOnce(manager, &tcpFake, ProtocolType::TCP_DIY,
                                                             CommissionRequest{"lamp2", "192.168.0.2", "s", "p"}, {}, notCancelled);

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(tcpFake.commissionActiveIds.size(), 1u);
    EXPECT_EQ(tcpFake.commissionActiveIds[0].count("TCP_000000000001"), 1u);
    EXPECT_EQ(tcpFake.commissionRequests[0].payload, "192.168.0.2");
    Device device;
    ASSERT_TRUE(manager.getDevice("TCP_000000000002", device));
    EXPECT_EQ(device.ip, "192.168.0.9");
}

// 작업 표뿐 아니라 로그에도 요청 비밀값이 없다(성공·실패·비밀값이 든 예외 메시지 모두)
TEST_F(CommissioningServiceTest, LogsNeverContainRequestSecrets) {
    const std::string password = "pw-SECRET-123";
    const std::string ssid = "MyHomeSSID";
    const std::string code = "34970112332";
    LogCapture capture;
    capture.start();  // 작업 스레드를 띄우기 전에
    service.start();

    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"SN-NEW"});
    auto ok = service.submit(ProtocolType::MATTER, CommissionRequest{"plug", code, ssid, password}, {});
    waitFinished(*ok.value);
    matterFake.commissionResult = Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "x");
    auto failed = service.submit(ProtocolType::MATTER, CommissionRequest{"plug", code, ssid, password}, {});
    waitFinished(*failed.value);
    matterFake.onCommission = [&] { throw std::runtime_error("bad " + password + " / " + ssid + " / " + code); };
    auto thrown = service.submit(ProtocolType::MATTER, CommissionRequest{"plug", code, ssid, password}, {});
    EXPECT_EQ(waitFinished(*thrown.value).errorCode, ErrorCode::CommissionFailed);

    service.stop();  // 작업 스레드 join 뒤에 되돌린다
    capture.stop();
    const std::string logs = capture.text();
    EXPECT_NE(logs.find("커미셔닝 중 예외"), std::string::npos);
    EXPECT_EQ(logs.find(password), std::string::npos);
    EXPECT_EQ(logs.find(ssid), std::string::npos);
    EXPECT_EQ(logs.find(code), std::string::npos);
}

// 등록 단계로 넘어갈 때는 비밀값을 비운다(드라이버는 원래 값을 받음)
TEST_F(CommissioningServiceTest, CommissionOnceClearsSecretsBeforeRegistration) {
    matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"SN-NEW"});
    const std::atomic<bool> notCancelled{false};
    const auto result = CommissioningService::commissionOnce(manager, &matterFake, ProtocolType::MATTER,
                                                             CommissionRequest{"plug", "34970112332", "ssid", "pw"}, {}, notCancelled);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(matterFake.commissionRequests[0].password, "pw");
    Device device;
    ASSERT_TRUE(manager.getDevice("SN-NEW", device));
    EXPECT_EQ(device.name, "plug");  // 이름은 남긴다
}

// Q6: 접수와 취소 요청이 겹쳐도 "취소가 끝난 뒤 접수된 작업"은 없다(TSan)
TEST_F(CommissioningServiceTest, SubmitRacingRequestCancelNeverAcceptsAfterCancel) {
    service.start();
    std::atomic<bool> cancelDone{false};
    std::atomic<int> acceptedAfterCancel{0};
    std::thread submitter([&] {
        for (int i = 0; i < 2000; ++i) {
            const bool cancelWasDone = cancelDone;
            const auto jobId = submitPlug();
            if (jobId.ok() && cancelWasDone) ++acceptedAfterCancel;
            if (!jobId.ok() && jobId.error.code == ErrorCode::Cancelled) break;
        }
    });
    std::this_thread::sleep_for(1ms);
    service.requestCancel();
    cancelDone = true;
    submitter.join();
    EXPECT_EQ(acceptedAfterCancel.load(), 0);
    EXPECT_EQ(submitPlug().error.code, ErrorCode::Cancelled);
}
