#pragma once
#include <string>

class IMessageSender {
public:
    virtual ~IMessageSender() = default;
    
    // 대상 ID와 명령어를 전달받아 전송을 수행하는 순수 가상 함수
    virtual void SendCommandTo(const std::string& target_id, const std::string& command) = 0;
};