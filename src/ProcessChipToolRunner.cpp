#include "ProcessChipToolRunner.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <pthread.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::chrono::milliseconds kPollSlice{100};  // 취소 플래그를 이 간격으로 확인한다
constexpr std::chrono::milliseconds kReapPoll{10};

// 범위를 벗어날 때 정리 동작을 실행한다(예외가 나도). dismiss()하면 실행하지 않는다
template <typename Action>
class ScopeExit {
public:
    explicit ScopeExit(Action action) : action_(std::move(action)) {}
    ~ScopeExit() { if (active_) action_(); }
    void dismiss() { active_ = false; }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
private:
    Action action_;
    bool active_ = true;
};

// 그룹(pgid)에 리더 말고 살아 있는 프로세스가 있는지 /proc에서 찾는다.
// 리더가 좀비여도 kill(-pgid, 0)은 성공하므로 그것만으로는 "손자가 남았는지" 알 수 없다
bool groupHasOtherMembers(pid_t pgid) {
    DIR* proc = ::opendir("/proc");
    if (!proc) return false;
    bool found = false;
    while (dirent* entry = ::readdir(proc)) {
        if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0]))) continue;
        const pid_t candidate = static_cast<pid_t>(std::atoi(entry->d_name));
        if (candidate == pgid) continue;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(candidate));
        FILE* stat = std::fopen(path, "re");
        if (!stat) continue;
        char line[512] = {0};
        const bool readOk = std::fgets(line, sizeof(line), stat) != nullptr;
        std::fclose(stat);
        if (!readOk) continue;
        // 형식: pid (comm) state ppid pgrp ... — comm에 공백·괄호가 있을 수 있어 마지막 ')' 뒤부터 읽는다
        const char* afterName = std::strrchr(line, ')');
        if (!afterName) continue;
        char state = 0;
        int parent = 0, group = 0;
        if (std::sscanf(afterName + 1, " %c %d %d", &state, &parent, &group) == 3 && group == pgid && state != 'Z') {
            found = true;
            break;
        }
    }
    ::closedir(proc);
    return found;
}
void closeIfOpen(int& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

// 회수하지 않고(WNOWAIT) 끝났는지만 본다. 회수 전까지 pid가 재사용되지 않으므로 그동안은 kill이 안전하다
bool hasExited(pid_t pid) {
    siginfo_t info{};
    info.si_pid = 0;
    if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0) return errno == ECHILD;
    return info.si_pid != 0;
}

// 쓴 바이트 수(EAGAIN이면 0), 상대가 닫았거나 오류면 -1.
// 파이프에는 MSG_NOSIGNAL이 없으므로, 이 스레드에서만 SIGPIPE를 잠시 막고 생긴 신호는 거둬들인다
ssize_t writeWithoutSigpipe(int fd, const char* data, std::size_t size) {
    sigset_t pipeSignal, previous;
    sigemptyset(&pipeSignal);
    sigaddset(&pipeSignal, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSignal, &previous);
    ssize_t written = ::write(fd, data, size);
    const int savedErrno = errno;
    if (written < 0 && savedErrno == EPIPE) {
        timespec noWait{0, 0};
        sigtimedwait(&pipeSignal, nullptr, &noWait);
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (written < 0) return (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK || savedErrno == EINTR) ? 0 : -1;
    return written;
}

int toExitCode(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

}  // namespace

std::string ProcessChipToolRunner::maskSecrets(std::string text, const std::vector<std::string>& secrets) {
    const std::string mask = "***";
    // 긴 것부터 가린다. 짧은 값("1234")이 긴 값("p1234word")의 일부면, 짧은 것을 먼저 가릴 때 긴 값이 조각나
    // "p***word"처럼 남고 긴 값으로는 더 이상 찾을 수 없게 된다
    std::vector<std::string> ordered(secrets.begin(), secrets.end());
    std::sort(ordered.begin(), ordered.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const auto& secret : ordered) {
        if (secret.empty()) continue;  // 빈 문자열은 무한 반복을 일으키므로 건너뛴다
        std::size_t position = 0;
        while ((position = text.find(secret, position)) != std::string::npos) {
            text.replace(position, secret.size(), mask);
            position += mask.size();
        }
    }
    return text;
}

ProcessResult ProcessChipToolRunner::run(const std::vector<std::string>& args, const std::string& stdinText,
                                         std::chrono::milliseconds timeout,
                                         const std::vector<std::string>& secretsToMask) {
    ProcessResult result;
    if (cancelled_) {
        result.cancelled = true;
        return result;
    }
    if (args.empty()) {
        result.startFailed = true;
        return result;
    }

    // fork한 자식은 다른 스레드가 쥔 malloc 락을 물려받을 수 있어 메모리를 할당하면 안 된다. argv는 fork 전에 만든다
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    int outputPipe[2] = {-1, -1};
    int childStdin = -1;
    int stdinWriter = -1;
    // 어떤 경로로 나가든(예외 포함) 부모 쪽 fd를 닫는다
    ScopeExit closeAll([&] {
        closeIfOpen(outputPipe[0]);
        closeIfOpen(outputPipe[1]);
        closeIfOpen(childStdin);
        closeIfOpen(stdinWriter);
    });
    if (::pipe2(outputPipe, O_CLOEXEC) != 0) {
        std::cerr << "[ChipToolRunner] 파이프 생성 실패" << std::endl;
        result.startFailed = true;
        return result;
    }
    // 읽기 쪽을 논블로킹으로: poll이 준비됐다고 해도 읽을 것이 없을 때 막히지 않게 한다
    ::fcntl(outputPipe[0], F_SETFL, ::fcntl(outputPipe[0], F_GETFL) | O_NONBLOCK);
    if (stdinText.empty()) {
        childStdin = ::open("/dev/null", O_RDONLY | O_CLOEXEC);  // 서버의 표준 입력을 물려주지 않는다
    } else {
        int inputPipe[2] = {-1, -1};
        if (::pipe2(inputPipe, O_CLOEXEC) == 0) {
            childStdin = inputPipe[0];
            stdinWriter = inputPipe[1];
            ::fcntl(stdinWriter, F_SETFL, ::fcntl(stdinWriter, F_GETFL) | O_NONBLOCK);
        }
    }
    if (childStdin < 0) {
        std::cerr << "[ChipToolRunner] 표준 입력 준비 실패" << std::endl;
        result.startFailed = true;
        return result;
    }

    pid_t pid = -1;
    {
        // fork와 등록을 한 락 안에서 해야 cancelAll이 막 생긴 자식을 놓치지 않는다
        std::lock_guard<std::mutex> lock(childrenMutex_);
        if (!cancelled_) {
            pid = ::fork();
            if (pid == 0) {
                // 자식: 여기서는 신호 안전 함수만 쓴다. dup2된 0·1·2번은 CLOEXEC가 풀려 exec 뒤에도 남는다
                ::setpgid(0, 0);
                // 서버가 0~2번을 닫은 채 돌고 있으면 파이프가 0~2번을 받았을 수 있다. 그대로 dup2하면
                // 순서에 따라 서로 덮어쓰므로, 먼저 3번 이상으로 옮긴다(CLOEXEC라 exec 뒤에는 남지 않는다)
                int inputFd = childStdin;
                int outputFd = outputPipe[1];
                if (inputFd <= STDERR_FILENO && (inputFd = ::fcntl(inputFd, F_DUPFD_CLOEXEC, 3)) < 0) ::_exit(127);
                if (outputFd <= STDERR_FILENO && (outputFd = ::fcntl(outputFd, F_DUPFD_CLOEXEC, 3)) < 0) ::_exit(127);
                if (::dup2(inputFd, STDIN_FILENO) < 0 || ::dup2(outputFd, STDOUT_FILENO) < 0 ||
                    ::dup2(outputFd, STDERR_FILENO) < 0) {
                    ::_exit(127);
                }
                ::execv(argv[0], argv.data());
                ::_exit(127);  // exec 실패. 부모의 버퍼를 비우지 않도록 _exit
            }
            if (pid > 0) {
                ::setpgid(pid, pid);  // 자식이 setpgid하기 전에 부모가 kill(-pid)해도 되도록 양쪽에서 설정
                children_.insert(pid);
            }
        }
    }
    closeIfOpen(outputPipe[1]);
    closeIfOpen(childStdin);
    if (pid <= 0) {
        if (pid < 0) {
            std::cerr << "[ChipToolRunner] 프로세스 생성 실패" << std::endl;
            result.startFailed = true;
        } else {
            result.cancelled = true;
        }
        return result;
    }

    // 어떤 경로로 나가든(예외 포함) 자식을 끝내고 회수하고 목록에서 뺀다
    bool reaped = false;
    ScopeExit reapChild([&] {
        if (!reaped) terminateAndReap(pid);
    });

    const auto deadline = Clock::now() + timeout;
    std::size_t stdinWritten = 0;
    bool truncated = false;
    bool outputOpen = true;
    char buffer[4096];
    while (outputOpen) {
        if (cancelled_) {
            result.cancelled = true;
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            result.timedOut = true;
            break;
        }
        pollfd fds[2] = {{outputPipe[0], POLLIN, 0}, {stdinWriter, POLLOUT, 0}};
        const nfds_t count = stdinWriter >= 0 ? 2 : 1;
        const int ready = ::poll(fds, count, static_cast<int>(std::min(remaining, kPollSlice).count()));
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (stdinWriter >= 0 && fds[1].revents != 0) {
            const ssize_t n = writeWithoutSigpipe(stdinWriter, stdinText.data() + stdinWritten, stdinText.size() - stdinWritten);
            if (n < 0) {
                closeIfOpen(stdinWriter);  // 자식이 입력을 닫았다
            } else {
                stdinWritten += static_cast<std::size_t>(n);
                if (stdinWritten == stdinText.size()) closeIfOpen(stdinWriter);  // EOF를 알린다
            }
        }
        if (fds[0].revents != 0) {
            const ssize_t n = ::read(outputPipe[0], buffer, sizeof(buffer));
            if (n > 0) {
                // 상한을 넘으면 더 쌓지 않고 읽어서 버린다(자식이 파이프가 차서 멈추지 않게)
                const std::size_t room = kMaxOutputBytes > result.output.size() ? kMaxOutputBytes - result.output.size() : 0;
                const std::size_t keep = std::min(room, static_cast<std::size_t>(n));
                result.output.append(buffer, keep);
                if (keep < static_cast<std::size_t>(n)) truncated = true;
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
                outputOpen = false;
            }
        }
    }
    closeIfOpen(outputPipe[0]);
    closeIfOpen(stdinWriter);
    if (truncated) {
        std::cerr << "[ChipToolRunner] 출력이 상한(" << kMaxOutputBytes << "바이트)을 넘어 잘랐습니다" << std::endl;
        result.output += kTruncatedMarker;
    }
    // cancelAll의 SIGTERM으로 자식이 먼저 끝나면 출력이 닫혀 위 반복이 정상 종료처럼 빠져나온다
    if (cancelled_ && !result.timedOut) result.cancelled = true;

    // 출력은 닫았지만 아직 실행 중일 수 있으므로 남은 시간 안에서 종료를 기다린다
    if (!result.timedOut && !result.cancelled) {
        while (!hasExited(pid)) {
            if (cancelled_) {
                result.cancelled = true;
                break;
            }
            if (Clock::now() >= deadline) {
                result.timedOut = true;
                break;
            }
            std::this_thread::sleep_for(kReapPoll);
        }
    }
    if (result.timedOut) {
        std::cerr << "[ChipToolRunner] 제한 시간(" << timeout.count() << "ms) 초과, 프로세스를 종료합니다" << std::endl;
    }
    const int status = terminateAndReap(pid);
    reaped = true;
    result.exitCode = toExitCode(status);
    result.output = maskSecrets(std::move(result.output), secretsToMask);
    return result;
}

int ProcessChipToolRunner::terminateAndReap(pid_t pid) {
    // 리더가 이미 끝났어도 손자 프로세스가 그룹에 남아 있을 수 있다. 리더를 회수하기 전(좀비로 남아 있어
    // 그룹 번호가 재사용되지 않는 동안)에 그룹 전체를 끝낸다. 그룹이 이미 비었으면 ESRCH는 무시한다
    auto groupAlive = [pid] { return !hasExited(pid) || groupHasOtherMembers(pid); };
    if (groupAlive()) {
        ::kill(-pid, SIGTERM);
        const auto graceDeadline = Clock::now() + kTerminateGrace;
        while (groupAlive() && Clock::now() < graceDeadline) std::this_thread::sleep_for(kReapPoll);
        if (groupAlive()) ::kill(-pid, SIGKILL);
    }
    // 끝날 때까지(회수는 아직) 기다린 뒤 목록에서 빼고 회수한다. 목록에 있는 동안은 pid가 재사용되지 않는다
    siginfo_t info{};
    while (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT) != 0 && errno == EINTR) {}
    {
        std::lock_guard<std::mutex> lock(childrenMutex_);
        children_.erase(pid);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return status;
}

void ProcessChipToolRunner::cancelAll() {
    cancelled_ = true;
    // kill은 막히지 않는 시스템 호출이다. 락 안에서 보내야 "회수되어 pid가 재사용된 뒤 엉뚱한 프로세스에 신호"가 생기지 않는다
    std::lock_guard<std::mutex> lock(childrenMutex_);
    for (pid_t pid : children_) ::kill(-pid, SIGTERM);
}
