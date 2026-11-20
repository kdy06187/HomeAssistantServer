#pragma once
#include "Result.hpp"
#include <string>

enum class CommandOutcome { Succeeded, Accepted, Failed };

// Accepted는 하드웨어 실행이 delaySeconds 뒤로 미뤄졌다는 뜻이다.
// state는 항상 채운다: 성공은 새 상태, 실패는 OFFLINE, 미등록은 UNKNOWN, 차단·지연·드라이버 없음은 현재 상태.
struct CommandResult {
    CommandOutcome outcome = CommandOutcome::Failed;
    Error error;
    std::string state;
    int delaySeconds = 0;
};
