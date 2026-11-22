#include <gtest/gtest.h>
#include "ProcessChipToolRunner.hpp"
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <set>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// 셸은 러너 쪽이 아니라 "테스트용 자식 프로그램"으로만 쓴다(2>&1 합치기·신호 무시 같은 상황을 만들려고)
std::vector<std::string> sh(const std::string& script) { return {"/bin/sh", "-c", script}; }

TEST(ProcessChipToolRunnerTest, RunsProgramWithArgumentsWithoutShell) {
    ProcessChipToolRunner runner;
    // 셸을 거치지 않으므로 ;, $(), 따옴표가 해석되지 않고 인자 한 개 그대로 전달된다
    const auto result = runner.run({"/bin/echo", "a; echo injected $(id) 'q'"}, "", 2000ms);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.output, "a; echo injected $(id) 'q'\n");
    EXPECT_FALSE(result.timedOut);
}

TEST(ProcessChipToolRunnerTest, StdoutAndStderrAreMergedLikeShellRedirect) {
    ProcessChipToolRunner runner;
    const auto result = runner.run(sh("echo out; echo err 1>&2"), "", 2000ms);
    EXPECT_NE(result.output.find("out"), std::string::npos);
    EXPECT_NE(result.output.find("err"), std::string::npos);
}

TEST(ProcessChipToolRunnerTest, ReportsExitCodes) {
    ProcessChipToolRunner runner;
    EXPECT_EQ(runner.run({"/bin/false"}, "", 2000ms).exitCode, 1);
    EXPECT_EQ(runner.run(sh("exit 3"), "", 2000ms).exitCode, 3);
    EXPECT_EQ(runner.run({"/nonexistent/chip-tool"}, "", 2000ms).exitCode, 127);  // exec 실패
}

TEST(ProcessChipToolRunnerTest, StdinTextIsDelivered) {
    ProcessChipToolRunner runner;
    const auto result = runner.run({"/bin/cat"}, "line1\nline2\n", 2000ms);
    EXPECT_EQ(result.output, "line1\nline2\n");
}

// 파이프 버퍼(64KiB)보다 큰 입력도 읽기와 번갈아 처리해 교착 없이 끝난다
TEST(ProcessChipToolRunnerTest, LargeStdinDoesNotDeadlock) {
    ProcessChipToolRunner runner;
    const std::string big(300 * 1024, 'z');
    const auto result = runner.run({"/bin/cat"}, big, 5000ms);
    EXPECT_FALSE(result.timedOut);
    EXPECT_EQ(result.output.size(), big.size());
}

// stdinText가 비면 /dev/null을 준다: 서버의 표준 입력을 기다리며 멈추지 않는다
TEST(ProcessChipToolRunnerTest, EmptyStdinIsDevNull) {
    ProcessChipToolRunner runner;
    const auto started = std::chrono::steady_clock::now();
    const auto result = runner.run({"/bin/cat"}, "", 2000ms);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1000ms);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(result.output.empty());
}

TEST(ProcessChipToolRunnerTest, TimeoutTerminatesWithSigterm) {
    ProcessChipToolRunner runner;
    const auto started = std::chrono::steady_clock::now();
    const auto result = runner.run({"/bin/sleep", "10"}, "", 300ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(result.timedOut);
    EXPECT_EQ(result.exitCode, 128 + SIGTERM);
    EXPECT_LT(elapsed, 1500ms);
}

// SIGTERM·SIGINT를 막은 스레드(서버의 main이 만든 스레드와 같은 상태)에서 실행해도
// 자식은 마스크를 물려받지 않아 SIGTERM으로 끝나야 한다(SIGKILL까지 가면 마스크가 남은 것)
void blockTerminationSignalsInThisThread() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
}

TEST(ProcessChipToolRunnerTest, ChildDoesNotInheritBlockedTermSignalOnTimeout) {
    ProcessChipToolRunner runner;
    auto result = std::async(std::launch::async, [&] {
        blockTerminationSignalsInThisThread();
        return runner.run({"/bin/sleep", "10"}, "", 300ms);
    }).get();
    EXPECT_TRUE(result.timedOut);
    EXPECT_EQ(result.exitCode, 128 + SIGTERM);
}

TEST(ProcessChipToolRunnerTest, ChildDoesNotInheritBlockedTermSignalOnCancelAll) {
    ProcessChipToolRunner runner;
    auto running = std::async(std::launch::async, [&] {
        blockTerminationSignalsInThisThread();
        return runner.run({"/bin/sleep", "10"}, "", 10000ms);
    });
    std::this_thread::sleep_for(300ms);
    const auto started = std::chrono::steady_clock::now();
    runner.cancelAll();
    const auto result = running.get();
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(result.exitCode, 128 + SIGTERM);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 900ms); // SIGKILL 유예(1초)까지 가지 않는다
}

// /proc/self/status의 SigIgn(무시)·SigBlk(막음) 마스크로 자식의 신호 상태를 직접 본다(셸 없이 grep만 사용)
unsigned long long signalMaskOf(const std::string& output, const std::string& field) {
    const auto pos = output.find(field + ":");
    if (pos == std::string::npos) return ~0ULL;
    return std::stoull(output.substr(pos + field.size() + 1), nullptr, 16);
}

TEST(ProcessChipToolRunnerTest, ChildStartsWithDefaultSigpipeAndNoBlockedSignals) {
    ProcessChipToolRunner runner;
    auto previous = std::signal(SIGPIPE, SIG_IGN); // 서버와 같은 상태
    const auto result = std::async(std::launch::async, [&] {
        blockTerminationSignalsInThisThread();
        return runner.run({"/bin/grep", "-E", "^Sig(Ign|Blk):", "/proc/self/status"}, "", 3000ms);
    }).get();
    std::signal(SIGPIPE, previous);

    ASSERT_EQ(result.exitCode, 0) << result.output;
    const unsigned long long sigpipeBit = 1ULL << (SIGPIPE - 1);
    EXPECT_EQ(signalMaskOf(result.output, "SigIgn") & sigpipeBit, 0ULL) << result.output;
    EXPECT_EQ(signalMaskOf(result.output, "SigBlk"), 0ULL) << result.output;
}

// SIGTERM을 무시하는 프로그램은 1초 유예 뒤 SIGKILL로 끝낸다
TEST(ProcessChipToolRunnerTest, IgnoredSigtermEscalatesToSigkill) {
    ProcessChipToolRunner runner;
    const auto started = std::chrono::steady_clock::now();
    const auto result = runner.run(sh("trap '' TERM; sleep 10"), "", 200ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(result.timedOut);
    EXPECT_EQ(result.exitCode, 128 + SIGKILL);
    EXPECT_GE(elapsed, ProcessChipToolRunner::kTerminateGrace);
    EXPECT_LT(elapsed, 3000ms);
}

// 프로세스 그룹 전체를 끝내므로 자식이 만든 손자 프로세스도 남지 않는다
TEST(ProcessChipToolRunnerTest, TimeoutKillsWholeProcessGroup) {
    ProcessChipToolRunner runner;
    const auto result = runner.run(sh("echo $$; sleep 30 & wait"), "", 300ms);
    ASSERT_TRUE(result.timedOut);
    const pid_t groupId = static_cast<pid_t>(std::stol(result.output));
    std::this_thread::sleep_for(100ms);
    errno = 0;
    EXPECT_EQ(::kill(-groupId, 0), -1);
    EXPECT_EQ(errno, ESRCH) << "그룹 " << groupId << "에 프로세스가 남아 있다";
}

// 출력을 닫고도 계속 도는 프로그램은 남은 시간 안에서 기다린 뒤 끝낸다
TEST(ProcessChipToolRunnerTest, ProgramThatClosesOutputButKeepsRunningIsStillBounded) {
    ProcessChipToolRunner runner;
    const auto started = std::chrono::steady_clock::now();
    const auto result = runner.run(sh("exec 1>&- 2>&-; sleep 10"), "", 300ms);
    EXPECT_TRUE(result.timedOut);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1500ms);
}

TEST(ProcessChipToolRunnerTest, SecretsAreMaskedInOutput) {
    ProcessChipToolRunner runner;
    const auto result = runner.run({"/bin/echo", "pw=secret123 pin=20202021 again secret123"}, "", 2000ms,
                                   {"secret123", "20202021", ""});
    EXPECT_EQ(result.output, "pw=*** pin=*** again ***\n");
}

TEST(ProcessChipToolRunnerTest, CancelAllStopsRunningChildAndRefusesNewRuns) {
    ProcessChipToolRunner runner;
    auto running = std::async(std::launch::async, [&] { return runner.run({"/bin/sleep", "10"}, "", 10000ms); });
    std::this_thread::sleep_for(200ms);
    const auto started = std::chrono::steady_clock::now();
    runner.cancelAll();
    const auto result = running.get();
    EXPECT_TRUE(result.cancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2000ms);

    const auto refused = runner.run({"/bin/echo", "x"}, "", 1000ms);
    EXPECT_TRUE(refused.cancelled);
    EXPECT_TRUE(refused.output.empty());
}

// 실행이 끝나면 자식이 모두 회수되어 좀비가 남지 않는다
TEST(ProcessChipToolRunnerTest, ChildrenAreAlwaysReaped) {
    ProcessChipToolRunner runner;
    for (int i = 0; i < 10; ++i) runner.run({"/bin/true"}, "", 1000ms);
    runner.run({"/bin/sleep", "5"}, "", 100ms);
    errno = 0;
    EXPECT_EQ(::waitpid(-1, nullptr, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD) << "회수되지 않은 자식이 있다";
}

// 러너가 만든 파이프는 CLOEXEC라 자식에 새지 않는다.
// 테스트 프로세스가 원래 CLOEXEC 없이 물려받은 fd(ctest가 넘긴 것 등)는 러너 책임이 아니므로 허용 목록에 넣는다
TEST(ProcessChipToolRunnerTest, RunnerPipesDoNotLeakIntoChild) {
    std::set<std::string> allowed = {"0", "1", "2"};
    if (DIR* dir = ::opendir("/proc/self/fd")) {
        const int dirFd = ::dirfd(dir);
        while (dirent* entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            const int fd = std::stoi(name);
            if (fd == dirFd) continue;
            const int flags = ::fcntl(fd, F_GETFD);
            if (flags >= 0 && !(flags & FD_CLOEXEC)) allowed.insert(name);
        }
        ::closedir(dir);
    }
    ProcessChipToolRunner runner;
    const auto result = runner.run({"/bin/ls", "/proc/self/fd"}, "", 2000ms);
    std::istringstream lines(result.output);
    std::string fd;
    int unexpectedCount = 0;
    std::string unexpected;
    while (std::getline(lines, fd)) {
        if (allowed.count(fd) == 0) {
            ++unexpectedCount;
            unexpected += fd + " ";
        }
    }
    // ls가 /proc/self/fd를 여는 디렉터리 fd 하나는 생긴다
    EXPECT_LE(unexpectedCount, 1) << "자식에 남은 fd: " << unexpected;
}

// 여러 스레드가 동시에 실행해도 서로의 출력이 섞이지 않는다(TSan)
TEST(ProcessChipToolRunnerTest, ConcurrentRunsKeepOutputsSeparate) {
    ProcessChipToolRunner runner;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 6; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < 8; ++i) {
                const std::string token = "T" + std::to_string(t) + "_" + std::to_string(i);
                if (runner.run({"/bin/echo", token}, "", 3000ms).output != token + "\n") ++mismatches;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(mismatches.load(), 0);
}

TEST(ProcessChipToolRunnerTest, EmptyArgsFailToStart) {
    ProcessChipToolRunner runner;
    const auto result = runner.run({}, "", 1000ms);
    EXPECT_TRUE(result.startFailed);
    EXPECT_EQ(result.exitCode, -1);
}

// ---- 8c ----

// 출력은 1MiB까지만 모으고, 넘으면 자르고 표시를 붙인다(자식은 막히지 않고 끝난다)
TEST(ProcessChipToolRunnerTest, OutputIsCappedAndMarked) {
    ProcessChipToolRunner runner;
    const auto result = runner.run({"/usr/bin/head", "-c", "3000000", "/dev/zero"}, "", 5000ms);
    EXPECT_FALSE(result.timedOut);
    EXPECT_EQ(result.exitCode, 0);
    const std::string marker = ProcessChipToolRunner::kTruncatedMarker;
    ASSERT_EQ(result.output.size(), ProcessChipToolRunner::kMaxOutputBytes + marker.size());
    EXPECT_EQ(result.output.substr(result.output.size() - marker.size()), marker);
}

// 리더가 정상 종료해도 그룹에 남은 손자 프로세스는 끝낸다
TEST(ProcessChipToolRunnerTest, GrandchildLeftByNormallyExitingLeaderIsTerminated) {
    ProcessChipToolRunner runner;
    const auto started = std::chrono::steady_clock::now();
    const auto result = runner.run(sh("sleep 30 >/dev/null 2>&1 & echo $!"), "", 3000ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_EQ(result.exitCode, 0);
    const pid_t grandchild = static_cast<pid_t>(std::stol(result.output));
    errno = 0;
    EXPECT_EQ(::kill(grandchild, 0), -1);
    EXPECT_EQ(errno, ESRCH) << "손자 " << grandchild << "가 남아 있다";
    EXPECT_LT(elapsed, 1500ms);  // SIGTERM으로 바로 끝나므로 유예 시간을 다 쓰지 않는다
}

// 겹치는 비밀값(짧은 값이 긴 값의 일부)도 남김없이 가린다
TEST(ProcessChipToolRunnerTest, OverlappingSecretsAreMaskedLongestFirst) {
    EXPECT_EQ(ProcessChipToolRunner::maskSecrets("pw=p1234word pin=1234", {"1234", "p1234word", ""}), "pw=*** pin=***");
    EXPECT_EQ(ProcessChipToolRunner::maskSecrets("abcabc", {"abc", "abcabc"}), "***");
}

// 서버가 0~2번 fd를 닫은 채 돌아도(파이프가 0~2번을 받음) 자식의 표준 입출력이 올바르게 연결된다.
// 테스트 프로세스 자체의 0~2번을 건드리지 않도록 fork한 자식 안에서 확인하고 종료 코드로 알린다
TEST(ProcessChipToolRunnerTest, WorksWhenServerHasStandardFdsClosed) {
    const pid_t checker = ::fork();
    ASSERT_GE(checker, 0);
    if (checker == 0) {
        ::close(0);
        ::close(1);
        ::close(2);
        ProcessChipToolRunner runner;
        const auto result = runner.run({"/bin/cat"}, "piped\n", 2000ms);
        ::_exit(result.exitCode == 0 && result.output == "piped\n" ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(checker, &status, 0), checker);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

}  // namespace
