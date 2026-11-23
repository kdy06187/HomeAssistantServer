#pragma once
#include "IMessageSender.hpp"
#include "ProtocolDriver.hpp"
#include <functional>
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
    std::vector<bool> readStateManualFlags;                          // readDeviceState의 isManualRequest

    bool sendCommand(const std::string& deviceId, const std::string& command) override {
        if (onSendCommand) onSendCommand(deviceId);
        if (throwOnSendCommand) throw std::runtime_error("fake sendCommand failure");
        std::lock_guard<std::mutex> lock(recordMutex);
        sentCommands.emplace_back(deviceId, command);
        return sendCommandResult;
    }
    Result<CommissionOutcome> commission(const CommissionRequest& request,
                                         const std::unordered_set<std::string>& activeDeviceIds) override {
        std::lock_guard<std::mutex> lock(recordMutex);
        commissionRequests.push_back(request);
        commissionActiveIds.push_back(activeDeviceIds);
        return commissionResult;
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
