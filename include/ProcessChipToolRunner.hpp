#pragma once
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <sys/types.h>
#include "ChipToolRunner.hpp"

// fork + execv로 실행하는 실제 러너.
// - 자식은 새 프로세스 그룹을 만들어, 끝낼 때 그 그룹 전체에 신호를 보낸다.
// - stdout·stderr는 한 파이프로 받고(셸의 2>&1과 같음), 모든 fd는 O_CLOEXEC로 만들어 자식에 새지 않는다.
// - 제한 시간이 지나면 SIGTERM, 1초 안에 안 끝나면 SIGKILL. 어떤 경우에도 waitpid로 회수한다(좀비 방지).
class ProcessChipToolRunner : public ChipToolRunner {
public:
    static constexpr std::chrono::milliseconds kTerminateGrace{1000};

    ProcessResult run(const std::vector<std::string>& args, const std::string& stdinText,
                      std::chrono::milliseconds timeout,
                      const std::vector<std::string>& secretsToMask = {}) override;
    void cancelAll() override;

    // 출력에서 비밀값을 ***로 바꾼다(빈 문자열은 무시)
    static std::string maskSecrets(std::string text, const std::vector<std::string>& secrets);

private:
    // 그룹에 SIGTERM → grace 안에 안 끝나면 SIGKILL → 회수. 회수한 상태값을 돌려준다
    int terminateAndReap(pid_t pid);

    std::atomic<bool> cancelled_{false};
    std::mutex childrenMutex_;             // children_만 보호(kill은 아래 이유로 이 락 안에서 한다)
    std::unordered_set<pid_t> children_;   // 아직 회수하지 않은 자식(프로세스 그룹 ID = pid)
};
