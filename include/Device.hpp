#pragma once
#include <string>
#include <vector>
#include <memory>
#include "IDeviceDriver.hpp"

// 통신 프로토콜 열거형
enum class ProtocolType {
    MATTER,
    TCP_DIY,
    UNKNOWN,
};

// 순수 데이터 보관용 기기 구조체
struct Device {
    std::string id;
    std::string name;
    ProtocolType protocol_type;
    std::string state;

    // 🌟 핵심: 기기에 부착된 특수 드라이버들을 담아두는 배열
    // (이 드라이버들을 꺼내서 실행하는 역할은 DriverManager가 담당합니다)
    std::vector<std::shared_ptr<IDeviceDriver>> attached_drivers;

    // 기본 생성자
    Device() : protocol_type(ProtocolType::UNKNOWN), state("OFF") {}

    // 값 초기화용 생성자
    Device(std::string id, std::string name, ProtocolType type)
        : id(std::move(id)), name(std::move(name)), protocol_type(type), state("OFF") {}
};