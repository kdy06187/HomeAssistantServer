#pragma once
#include "IMessageSender.hpp"
#include "ProtocolDriver.hpp"
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// 호출을 기록하고, 반환값·예외를 테스트가 정하는 가짜 프로토콜 드라이버.
// PCDriver가 쓰는 IMessageSender도 겸한다.
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

    // 기록
    std::vector<std::pair<std::string, std::string>> sentCommands;   // (deviceId, command)
    std::vector<CommissionRequest> commissionRequests;
    std::vector<std::unordered_set<std::string>> commissionActiveIds;
    std::vector<std::string> unpairedIds;
    std::vector<std::pair<std::string, std::string>> sentMessages;   // (target, payload)

    bool sendCommand(const std::string& deviceId, const std::string& command) override {
        sentCommands.emplace_back(deviceId, command);
        return sendCommandResult;
    }
    Result<CommissionOutcome> commission(const CommissionRequest& request,
                                         const std::unordered_set<std::string>& activeDeviceIds) override {
        commissionRequests.push_back(request);
        commissionActiveIds.push_back(activeDeviceIds);
        return commissionResult;
    }
    bool unpairDevice(const std::string& deviceId) override {
        unpairedIds.push_back(deviceId);
        return unpairResult;
    }
    std::string readDeviceState(const std::string&, bool) override {
        if (throwOnReadState) throw std::runtime_error("fake readDeviceState failure");
        return stateToReturn;
    }
    Result<EnergyReading> readEnergy(const std::string&) override {
        if (throwOnReadEnergy) throw std::runtime_error("fake readEnergy failure");
        return energyResult;
    }

    void SendCommandTo(const std::string& target_id, const std::string& command) override {
        sentMessages.emplace_back(target_id, command);
    }
};
