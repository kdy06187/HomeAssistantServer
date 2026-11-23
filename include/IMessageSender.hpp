#pragma once
#include <string>
#include "Result.hpp"

// 기기 드라이버(PCDriver 등)가 기기로 메시지를 보낼 때 쓰는 가벼운 인터페이스. TCP 쪽(TcpMessageSender)만 구현한다.
class IMessageSender {
public:
    virtual ~IMessageSender() = default;

    // 한 기기에 메시지 한 줄(JSON 문자열)을 보낸다
    virtual Status sendTo(const std::string& deviceId, const std::string& jsonLine) = 0;
    // PC 에이전트에게 보낸다(에이전트 설정이 없으면 연결된 모든 기기에). 결과는 로그로만 남긴다
    virtual void sendToPcAgent(const std::string& jsonLine) = 0;
};
