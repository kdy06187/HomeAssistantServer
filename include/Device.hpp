#pragma once
#include <string>

enum class ProtocolType{
    MATTER,
    TCP_DIY,
    UNKNOWN,
};

struct Device{
    std::string id;
    std::string name;
    ProtocolType protocol_type;
    std::string state;
};