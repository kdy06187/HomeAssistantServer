#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <thread>
#include "DelayedCommandScheduler.hpp"
#include "fakes/FakeClock.hpp"

using namespace std::chrono_literals;

namespace {
// 시각 형식 시험이 로컬 시간대에 흔들리지 않게 UTC로 고정한다
class SchedulerTestEnv : public ::testing::Environment {
public:
    void SetUp() override {
        setenv("TZ", "UTC", 1);
        tzset();
    }
};
const auto* const kEnv = ::testing::AddGlobalTestEnvironment(new SchedulerTestEnv);
}  // namespace

class DelayedCommandSchedulerTest : public ::testing::Test {
protected:
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    int runCount = 0;
    DelayedCommandScheduler::Task countTask() { return [this] { ++runCount; }; }
};

TEST_F(DelayedCommandSchedulerTest, 만기_전에는_실행하지_않고_만기에_한_번만_실행한다) {
    ASSERT_TRUE(scheduler.schedule("d1", "OFF", 45s, countTask()));
    clock.advance(44s);
    scheduler.tick(clock.steadyNow());
    EXPECT_EQ(runCount, 0);
    clock.advance(1s);
    scheduler.tick(clock.steadyNow());
    EXPECT_EQ(runCount, 1);
    scheduler.tick(clock.steadyNow());
    EXPECT_EQ(runCount, 1);
    EXPECT_FALSE(scheduler.pending("d1").has_value());
}

TEST_F(DelayedCommandSchedulerTest, 같은_기기의_두번째_schedule은_false이고_기존_작업을_유지한다) {
    ASSERT_TRUE(scheduler.schedule("d1", "OFF", 45s, countTask()));
    EXPECT_FALSE(scheduler.schedule("d1", "ON", 10s, countTask()));
    const auto pending = scheduler.pending("d1");
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->command, "OFF");
    EXPECT_EQ(pending->remainingSeconds, 45);
    EXPECT_TRUE(scheduler.schedule("d2", "OFF", 45s, countTask())); // 다른 기기는 독립
}

TEST_F(DelayedCommandSchedulerTest, cancel_뒤에는_실행하지_않는다) {
    scheduler.schedule("d1", "OFF", 45s, countTask());
    EXPECT_TRUE(scheduler.cancel("d1"));
    EXPECT_FALSE(scheduler.cancel("d1"));
    clock.advance(60s);
    scheduler.tick(clock.steadyNow());
    EXPECT_EQ(runCount, 0);
    EXPECT_TRUE(scheduler.schedule("d1", "OFF", 45s, countTask())); // 취소 뒤 다시 예약 가능
}

TEST_F(DelayedCommandSchedulerTest, pending의_남은_초는_올림이다) {
    scheduler.schedule("d1", "OFF", 45s, countTask());
    clock.advance(10s);
    EXPECT_EQ(scheduler.pending("d1")->remainingSeconds, 35);
    clock.advance(std::chrono::milliseconds(500));
    EXPECT_EQ(scheduler.pending("d1")->remainingSeconds, 35); // 34.5초 -> 35
    clock.advance(std::chrono::milliseconds(500));
    EXPECT_EQ(scheduler.pending("d1")->remainingSeconds, 34);
    EXPECT_FALSE(scheduler.pending("없는기기").has_value());
}

TEST_F(DelayedCommandSchedulerTest, stopAndCancelAll_뒤에는_예약_거부_미실행) {
    scheduler.schedule("d1", "OFF", 45s, countTask());
    scheduler.schedule("d2", "OFF", 45s, countTask());
    testing::internal::CaptureStdout();
    scheduler.stopAndCancelAll();
    const std::string log = testing::internal::GetCapturedStdout();
    EXPECT_NE(log.find("[Scheduler] 지연 OFF 취소: d1"), std::string::npos);
    EXPECT_NE(log.find("[Scheduler] 지연 OFF 취소: d2"), std::string::npos);
    EXPECT_NE(log.find("서버 종료"), std::string::npos);

    EXPECT_FALSE(scheduler.schedule("d3", "OFF", 1s, countTask()));
    clock.advance(60s);
    scheduler.tick(clock.steadyNow());
    EXPECT_EQ(runCount, 0);
    EXPECT_FALSE(scheduler.pending("d1").has_value());
}

TEST_F(DelayedCommandSchedulerTest, 작업_안에서_schedule과_cancel을_불러도_교착하지_않는다) {
    bool rescheduled = false;
    scheduler.schedule("d1", "OFF", 1s, [&] {
        rescheduled = scheduler.schedule("d1", "ON", 5s, [] {});
        scheduler.pending("d1");
        scheduler.cancel("d2");
    });
    clock.advance(1s);
    scheduler.tick(clock.steadyNow());
    EXPECT_TRUE(rescheduled);
    EXPECT_EQ(scheduler.pending("d1")->command, "ON");
}

TEST_F(DelayedCommandSchedulerTest, 만기가_같은_작업은_만기_순서로_실행한다) {
    std::string order;
    scheduler.schedule("a", "OFF", 20s, [&] { order += "a"; });
    scheduler.schedule("b", "OFF", 10s, [&] { order += "b"; });
    clock.advance(30s);
    scheduler.tick(clock.steadyNow());
    EXPECT_EQ(order, "ba");
}

TEST_F(DelayedCommandSchedulerTest, 작업이_예외를_던져도_다음_작업은_실행한다) {
    scheduler.schedule("a", "OFF", 1s, [] { throw std::runtime_error("실패"); });
    scheduler.schedule("b", "OFF", 2s, countTask());
    clock.advance(5s);
    testing::internal::CaptureStderr();
    scheduler.tick(clock.steadyNow());
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(runCount, 1);
}

TEST_F(DelayedCommandSchedulerTest, 로그에_예약_실행_취소와_시각이_남는다) {
    // utcNow 시작 = 1970-01-01 00:00:00 UTC
    testing::internal::CaptureStdout();
    scheduler.schedule("d1", "OFF", 45s, countTask());
    scheduler.schedule("d2", "OFF", 45s, countTask());
    scheduler.cancel("d2", "삭제");
    clock.advance(45s);
    scheduler.tick(clock.steadyNow());
    const std::string log = testing::internal::GetCapturedStdout();
    EXPECT_NE(log.find("[Scheduler] 지연 OFF 예약: d1, 실행 예정 00:00:45(45초 뒤)"), std::string::npos) << log;
    EXPECT_NE(log.find("[Scheduler] 지연 OFF 취소: d2, 00:00:00 (사유: 삭제)"), std::string::npos) << log;
    EXPECT_NE(log.find("[Scheduler] 지연 OFF 실행: d1, 00:00:45"), std::string::npos) << log;
}

TEST_F(DelayedCommandSchedulerTest, 만기가_지났지만_tick_전이면_pending은_0초다) {
    scheduler.schedule("d1", "OFF", 45s, countTask());
    clock.advance(50s);
    const auto pending = scheduler.pending("d1");
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->remainingSeconds, 0);
}

TEST_F(DelayedCommandSchedulerTest, 실행_중_작업이_있으면_stop이_기다리고_같은_시점의_다음_작업은_실행하지_않는다) {
    std::atomic<bool> firstStarted{false};
    std::atomic<bool> releaseFirst{false};
    std::atomic<bool> stopReturned{false};
    scheduler.schedule("a", "OFF", 1s, [&] {
        firstStarted = true;
        while (!releaseFirst) std::this_thread::sleep_for(1ms);
    });
    scheduler.schedule("b", "OFF", 2s, countTask());
    clock.advance(10s); // 둘 다 만기

    std::thread ticker([&] { scheduler.tick(clock.steadyNow()); });
    while (!firstStarted) std::this_thread::sleep_for(1ms);
    std::thread stopper([&] {
        scheduler.stopAndCancelAll();
        stopReturned = true;
    });
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(stopReturned); // 실행 중인 작업이 끝나기 전에는 돌아오지 않는다

    releaseFirst = true;
    stopper.join();
    ticker.join();
    EXPECT_TRUE(stopReturned);
    EXPECT_EQ(runCount, 0); // b는 실행되지 않았다
}

TEST(FormatClockTimeTest, 시각을_HH_MM_SS로_만든다) {
    EXPECT_EQ(formatClockTime(std::chrono::system_clock::time_point{}), "00:00:00");
    EXPECT_EQ(formatClockTime(std::chrono::system_clock::time_point{} + 3661s), "01:01:01");
}

TEST(DelayedCommandSchedulerThreadTest, 긴_예약_중_짧은_예약을_추가하면_바로_깨어나_실행한다) {
    SystemClock clock;
    DelayedCommandScheduler scheduler(clock);
    scheduler.start();
    std::atomic<int> runCount{0};
    scheduler.schedule("long", "OFF", std::chrono::hours(1), [] {});
    std::this_thread::sleep_for(50ms); // 스레드가 1시간짜리를 기준으로 잠든 뒤
    scheduler.schedule("short", "OFF", 50ms, [&] { ++runCount; });
    for (int i = 0; i < 200 && runCount == 0; ++i) std::this_thread::sleep_for(10ms);
    EXPECT_EQ(runCount, 1);
    scheduler.stopAndCancelAll();
}

TEST(DelayedCommandSchedulerThreadTest, 실제_스레드가_짧은_지연_뒤_작업을_실행한다) {
    SystemClock clock;
    DelayedCommandScheduler scheduler(clock);
    scheduler.start();
    std::atomic<int> runCount{0};
    scheduler.schedule("d1", "OFF", 50ms, [&] { ++runCount; });
    for (int i = 0; i < 200 && runCount == 0; ++i) std::this_thread::sleep_for(10ms);
    EXPECT_EQ(runCount, 1);
    scheduler.stopAndCancelAll();
}
