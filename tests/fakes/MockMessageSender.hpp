#pragma once
#include <gmock/gmock.h>
#include "IMessageSender.hpp"

class MockMessageSender : public IMessageSender {
public:
    MOCK_METHOD(void, SendCommandTo, (const std::string& target_id, const std::string& command), (override));
};
