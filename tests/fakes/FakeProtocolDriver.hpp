#pragma once
#include "IMessageSender.hpp"
#include "ProtocolDriver.hpp"
#include <chrono>
#include <functional>
#include <thread>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// 호출을 기록하고, 반환값·예외를 테스트가 정하는 가짜 프로토콜 드라이버.
// PCDriver가 쓰는 IMessageSender도 겸한다.
// 여러 스레드가 동시에 불러도 기록이 깨지지 않도록 기록은 recordMutex 안에서 한다.
// 설정 값은 스레드를 띄우기 전에 정하고, 기록은 스레드가 끝난 뒤에 읽는다.
class FakeProtocolDriver : public ProtocolDriver, public IMessageSender {
public:
    // 설정
    bool sendCommandResult = true;
    std::string stateToReturn = "ON";
    bool unpairResult = true;
    Result<CommissionOutcome> commissionResult =
        Result<CommissionOutcome>::success(CommissionOutcome{"FAKE_1"});
    Result<EnergyReading> energyResult = Result<EnergyReading>::success(EnergyReading{0, 0});
    bool throwOnReadState = false;
    bool throwOnReadEnergy = false;
    bool throwOnSendCommand = false;
    std::string thrownMessage = "fake sendCommand failure";  // throwOnSendCommand일 때 예외 문구(로그 sanitize 시험용)
    bool throwOnUnpair = false;
    // 호출 도중에 멈추게 하는 훅(교착 시험용). 기록 락 밖에서 호출되며 인자는 deviceId다.
    std::function<void()> onUnpair;
    std::function<void(const std::string&)> onSendCommand;
    std::function<void(const std::string&)> onReadState;
    // 설정하면 energyResult 대신 이 함수의 결과를 돌려준다(호출별 값·멈춤 조작용). 기록 락 밖에서 호출된다.
    std::function<Result<EnergyReading>(const std::string&)> onReadEnergy;

    // 기록
    std::vector<std::pair<std::string, std::string>> sentCommands;   // (deviceId, command)
    std::vector<CommissionRequest> commissionRequests;
    std::vector<std::unordered_set<std::string>> commissionActiveIds;
    std::vector<std::string> unpairedIds;
    std::vector<std::pair<std::string, std::string>> sentMessages;   // sendTo: (deviceId, jsonLine)
    std::vector<std::string> pcAgentMessages;                        // sendToPcAgent: jsonLine
    std::vector<std::string> energyReadIds;
    std::function<void()> onCommission;
    std::vector<std::pair<std::string, bool>> finalizedOutcomes;     // finalizeCommission: (deviceId, accepted)
    std::function<void(const CommissionOutcome&, bool)> onFinalize;  // 기록 락 밖에서 호출된다
    bool throwOnFinalize = false;
    bool ignoreCancel = false;  // 드라이버가 끝난 "직후" 종료가 시작되는 경우를 흉내 낸다
    bool waitForCancel = false; // 취소 플래그가 설 때까지(최대 5초) 기다린다: 진행 중 작업의 종료 시험용
    std::vector<bool> readStateManualFlags;                          // readDeviceState의 isManualRequest

    bool sendCommand(const std::string& deviceId, const std::string& command) override {
        if (onSendCommand) onSendCommand(deviceId);
        if (throwOnSendCommand) throw std::runtime_error(thrownMessage);
        std::lock_guard<std::mutex> lock(recordMutex);
        sentCommands.emplace_back(deviceId, command);
        return sendCommandResult;
    }
    Result<CommissionOutcome> commission(const CommissionRequest& request,
                                         const std::unordered_set<std::string>& activeDeviceIds,
                                         const std::atomic<bool>& cancelRequested) override {
        {
            std::lock_guard<std::mutex> lock(recordMutex);
            commissionRequests.push_back(request);
            commissionActiveIds.push_back(activeDeviceIds);
        }
        // 진행 중 취소를 시험할 때 쓴다(예: 이 안에서 beginShutdown). 끝난 뒤 플래그가 서 있으면 실제 드라이버처럼 취소
        if (onCommission) onCommission();
        for (int i = 0; waitForCancel && !cancelRequested && i < 500; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (cancelRequested && !ignoreCancel) return Result<CommissionOutcome>::failure(ErrorCode::Cancelled, "fake cancelled");
        return commissionResult;
    }
    void finalizeCommission(const CommissionOutcome& outcome, bool accepted) override {
        if (onFinalize) onFinalize(outcome, accepted);
        if (throwOnFinalize) throw std::runtime_error("fake finalizeCommission failure");
        std::lock_guard<std::mutex> lock(recordMutex);
        finalizedOutcomes.emplace_back(outcome.deviceId, accepted);
    }
    bool unpairDevice(const std::string& deviceId) override {
        if (onUnpair) onUnpair();
        if (throwOnUnpair) throw std::runtime_error("fake unpairDevice failure");
        std::lock_guard<std::mutex> lock(recordMutex);
        unpairedIds.push_back(deviceId);
        return unpairResult;
    }
    std::string readDeviceState(const std::string& deviceId, bool isManualRequest) override {
        {
            std::lock_guard<std::mutex> lock(recordMutex);
            readStateManualFlags.push_back(isManualRequest);
        }
        if (onReadState) onReadState(deviceId);
        if (throwOnReadState) throw std::runtime_error("fake readDeviceState failure");
        return stateToReturn;
    }
    Result<EnergyReading> readEnergy(const std::string& deviceId) override {
        if (throwOnReadEnergy) throw std::runtime_error("fake readEnergy failure");
        {
            std::lock_guard<std::mutex> lock(recordMutex);
            energyReadIds.push_back(deviceId);
        }
        if (onReadEnergy) return onReadEnergy(deviceId);
        std::lock_guard<std::mutex> lock(recordMutex);
        return energyResult;
    }

    Status sendTo(const std::string& deviceId, const std::string& jsonLine) override {
        std::lock_guard<std::mutex> lock(recordMutex);
        sentMessages.emplace_back(deviceId, jsonLine);
        return Status::success();
    }
    void sendToPcAgent(const std::string& jsonLine) override {
        std::lock_guard<std::mutex> lock(recordMutex);
        pcAgentMessages.push_back(jsonLine);
    }

    std::mutex recordMutex;
};
