#pragma once
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "MacResolver.hpp"

// 시험용 MAC 조회. 기본 규칙: 127.0.0.N → "0200000000" + N(16진 2자). 그 밖 IP는 없음.
// 수락 스레드가 부르는 동안 시험이 규칙을 바꿀 수 있어 락으로 보호한다.
class FakeMacResolver : public MacResolver {
public:
    static std::string loopbackMac(int lastOctet) {
        char suffix[3];
        std::snprintf(suffix, sizeof(suffix), "%02X", lastOctet & 0xFF);
        return std::string("0200000000") + suffix;
    }
    static std::string loopbackId(int lastOctet) { return makeTcpDeviceId(loopbackMac(lastOctet)); }

    std::optional<std::string> resolve(const std::string& peerIpv4) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++calls_;
        if (failing_.count(peerIpv4)) return std::nullopt;
        auto it = overrides_.find(peerIpv4);
        if (it != overrides_.end()) return it->second;
        const std::string prefix = "127.0.0.";
        if (peerIpv4.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
        return loopbackMac(std::stoi(peerIpv4.substr(prefix.size())));
    }

    // 그 IP를 다른 MAC으로 보이게 한다(IP가 바뀐 같은 기기 재접속 시험)
    void setMac(const std::string& ip, const std::string& mac) {
        std::lock_guard<std::mutex> lock(mutex_);
        overrides_[ip] = mac;
    }
    void setFailing(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mutex_);
        failing_.insert(ip);
    }
    int calls() {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::string> overrides_;
    std::unordered_set<std::string> failing_;
    int calls_ = 0;
};
