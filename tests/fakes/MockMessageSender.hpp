#pragma once
#include <gmock/gmock.h>
#include "IMessageSender.hpp"

class MockMessageSender : public IMessageSender {
public:
    MOCK_METHOD(Status, sendTo, (const std::string& deviceId, const std::string& jsonLine), (override));
    MOCK_METHOD(void, broadcast, (const std::string& jsonLine), (override));
};
