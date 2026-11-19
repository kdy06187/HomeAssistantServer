#pragma once
#include <chrono>
#include <string>
#include <vector>

// 외부 프로그램(chip-tool) 한 번 실행한 결과
struct ProcessResult {
    int exitCode = -1;        // 정상 종료면 종료 코드, 신호로 끝나면 128 + 신호 번호, 실행 못 했으면 -1
    std::string output;       // stdout + stderr(합쳐서), secretsToMask는 ***로 가려져 있다
    bool timedOut = false;    // 제한 시간이 지나 강제로 끝냈다
    bool cancelled = false;   // cancelAll로 끝냈거나 시작하지 않았다
    bool startFailed = false; // 파이프·fork 실패(이때 exitCode -1)
};

// chip-tool 실행을 감싼 어댑터. 테스트는 가짜로 바꿔 끼운다
class ChipToolRunner {
public:
    virtual ~ChipToolRunner() = default;

    // args[0]은 실행 파일 경로. 셸을 거치지 않는다.
    // stdinText는 자식의 표준 입력으로 보낸다(비어 있으면 /dev/null). 비밀값은 인자 대신 여기로만 보낸다(2차).
    virtual ProcessResult run(const std::vector<std::string>& args, const std::string& stdinText,
                              std::chrono::milliseconds timeout,
                              const std::vector<std::string>& secretsToMask = {}) = 0;

    // 이후 실행을 거부하고, 실행 중인 자식을 모두 끝낸다(SIGTERM → 1초 뒤 SIGKILL)
    virtual void cancelAll() = 0;
};
