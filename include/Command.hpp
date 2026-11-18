#pragma once
#include "Result.hpp"
#include <string>

enum class CommandOutcome { Succeeded, Accepted, Failed };

// Accepted는 하드웨어 실행이 delaySeconds 뒤로 미뤄졌다는 뜻이다. state는 이번 명령으로 바뀐 기기 상태(없으면 빈 문자열).
struct CommandResult {
    CommandOutcome outcome = CommandOutcome::Failed;
    Error error;
    std::string state;
    int delaySeconds = 0;
};
