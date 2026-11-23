#include "MacResolver.hpp"
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <thread>

namespace {

bool isHexDigit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

std::optional<std::string> readWholeFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) return std::nullopt;
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

}  // namespace

namespace MacResolverParse {

std::optional<std::string> normalizeMac(const std::string& text) {
    std::string hex;
    if (text.size() == 12) {
        hex = text;
    } else if (text.size() == 17) {
        // 구분자 위치는 2, 5, 8, 11, 14이고 모두 같은 문자(':' 또는 '-')여야 한다
        const char separator = text[2];
        if (separator != ':' && separator != '-') return std::nullopt;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (i % 3 == 2) {
                if (text[i] != separator) return std::nullopt;
            } else {
                hex.push_back(text[i]);
            }
        }
    } else {
        return std::nullopt;
    }
    for (char& c : hex) {
        if (!isHexDigit(c)) return std::nullopt;
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return hex;
}

std::optional<std::string> findArpMac(const std::string& arpText, const std::string& ipv4) {
    std::istringstream lines(arpText);
    std::string line;
    std::getline(lines, line);  // 머리글 "IP address HW type Flags HW address Mask Device"
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string ip, hwType, flags, mac;
        if (!(fields >> ip >> hwType >> flags >> mac)) continue;
        if (ip != ipv4) continue;  // 앞부분 일치(192.168.0.1 vs 192.168.0.10)를 막으려고 완전 일치만
        // 플래그 0x0은 응답을 아직 못 받은 미완성 항목(MAC이 0으로 채워져 있다)
        if (flags == "0x0") continue;
        auto normalized = normalizeMac(mac);
        if (!normalized || *normalized == "000000000000") continue;
        return normalized;
    }
    return std::nullopt;
}

std::optional<std::string> findDefaultGatewayIp(const std::string& routeText) {
    constexpr unsigned long kRouteFlagGateway = 0x2;  // RTF_GATEWAY
    std::istringstream lines(routeText);
    std::string line;
    std::getline(lines, line);  // 머리글
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string iface, destination, gateway, flags;
        if (!(fields >> iface >> destination >> gateway >> flags)) continue;
        if (destination != "00000000") continue;
        try {
            if ((std::stoul(flags, nullptr, 16) & kRouteFlagGateway) == 0) continue;
            if (gateway.size() != 8) continue;
            // 커널이 네트워크 바이트 순서 그대로의 32비트 값을 호스트(리틀 엔디언) 16진으로 찍는다:
            // "0100A8C0" → 바이트 01 00 A8 C0을 거꾸로 읽어 192.168.0.1
            const uint32_t value = static_cast<uint32_t>(std::stoul(gateway, nullptr, 16));
            std::ostringstream dotted;
            dotted << (value & 0xFF) << '.' << ((value >> 8) & 0xFF) << '.' << ((value >> 16) & 0xFF) << '.'
                   << ((value >> 24) & 0xFF);
            return dotted.str();
        } catch (const std::exception&) {
            continue;  // 16진이 아닌 칸은 그 줄만 건너뛴다
        }
    }
    return std::nullopt;
}

}  // namespace MacResolverParse

ProcNetArpResolver::ProcNetArpResolver(std::string arpPath, std::string routePath, int attempts,
                                       std::chrono::milliseconds retryInterval)
    : arpPath_(std::move(arpPath)), routePath_(std::move(routePath)),
      attempts_(attempts < 1 ? 1 : attempts), retryInterval_(retryInterval) {}

std::optional<std::string> ProcNetArpResolver::resolve(const std::string& peerIpv4) {
    for (int attempt = 0; attempt < attempts_; ++attempt) {
        // 핸드셰이크 직후 ARP 항목이 아직 미완성일 수 있어 잠깐 기다렸다 다시 읽는다
        if (attempt > 0) std::this_thread::sleep_for(retryInterval_);
        const auto arpText = readWholeFile(arpPath_);
        if (!arpText) continue;
        const auto mac = MacResolverParse::findArpMac(*arpText, peerIpv4);
        if (!mac) continue;

        // 라우터를 거쳐 온 접속은 ARP 표에 라우터 MAC으로 보인다. 그 MAC을 기기 ID로 쓰면 안 된다
        const auto routeText = readWholeFile(routePath_);
        const auto gatewayIp = routeText ? MacResolverParse::findDefaultGatewayIp(*routeText) : std::nullopt;
        if (gatewayIp) {
            const auto gatewayMac = MacResolverParse::findArpMac(*arpText, *gatewayIp);
            if (gatewayMac && *gatewayMac == *mac) return std::nullopt;
        }
        return mac;
    }
    return std::nullopt;
}
