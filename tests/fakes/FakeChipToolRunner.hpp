#pragma once
#include "ChipToolRunner.hpp"
#include <atomic>
#include <thread>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// 실제 chip-tool 대신 정해 둔 출력을 돌려주는 가짜 러너.
// respond는 하위 명령(args[1..])을 받아 출력을 만든다. 스레드를 띄우기 전에 정하고, 기록은 끝난 뒤 읽는다
class FakeChipToolRunner : public ChipToolRunner {
public:
    std::function<ProcessResult(const std::vector<std::string>& subcommand)> respond;

    ProcessResult run(const std::vector<std::string>& args, const std::string& stdinText,
                      std::chrono::milliseconds timeout, const std::vector<std::string>& secretsToMask) override {
        std::vector<std::string> subcommand(args.begin() + (args.empty() ? 0 : 1), args.end());
        {
            std::lock_guard<std::mutex> lock(recordMutex);
            calls.push_back(Call{args, stdinText, timeout, secretsToMask});
        }
        return respond ? respond(subcommand) : ProcessResult{0, "CHIP_NO_ERROR", false, false, false};
    }

    void cancelAll() override { cancelled = true; }

    struct Call {
        std::vector<std::string> args;
        std::string stdinText;
        std::chrono::milliseconds timeout;
        std::vector<std::string> secretsToMask;
    };
    std::vector<Call> callsSnapshot() {
        std::lock_guard<std::mutex> lock(recordMutex);
        return calls;
    }

    std::mutex recordMutex;
    std::vector<Call> calls;
    std::atomic<bool> cancelled{false};
};

// 자주 쓰는 chip-tool 출력(ChipToolOutputParser가 보는 표지)
namespace FakeChipToolOutput {
inline ProcessResult ok(const std::string& body) { return ProcessResult{0, "CHIP_NO_ERROR\n" + body, false, false, false}; }
inline ProcessResult timeout() { return ProcessResult{124, "Timeout waiting for response", true, false, false}; }
}  // namespace FakeChipToolOutput
