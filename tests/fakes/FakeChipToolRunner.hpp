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
        ++started;  // 기록보다 먼저 세야 "기록은 됐는데 아직 시작 전"인 틈에 대기가 끝나지 않는다
        ++running;
        {
            std::lock_guard<std::mutex> lock(recordMutex);
            calls.push_back(Call{args, stdinText, timeout, secretsToMask});
        }
        // 응답 함수가 예외를 던져도 "끝난 호출"로 센다
        struct Finish {
            FakeChipToolRunner& self;
            ~Finish() { --self.running; ++self.finished; }
        } finish{*this};
        return respond ? respond(subcommand) : ProcessResult{0, "CHIP_NO_ERROR", false, false, false};
    }

    // 시작한 호출이 expectedStarted개 이상이 되고, 시작한 만큼 모두 끝날 때까지 기다린다.
    // 분리 스레드(unpair)가 아직 run에 들어오지 않았을 수 있으므로 "시작 수"를 함께 기다린다
    bool waitUntilIdle(int expectedStarted = 0, std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (started.load() >= expectedStarted && started.load() == finished.load()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
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
    std::atomic<int> running{0};
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::atomic<bool> cancelled{false};
};

// 자주 쓰는 chip-tool 출력(ChipToolOutputParser가 보는 표지)
namespace FakeChipToolOutput {
inline ProcessResult ok(const std::string& body) { return ProcessResult{0, "CHIP_NO_ERROR\n" + body, false, false, false}; }
inline ProcessResult timeout() { return ProcessResult{124, "Timeout waiting for response", true, false, false}; }
}  // namespace FakeChipToolOutput
