#include "MacResolver.hpp"
#include <arpa/inet.h>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <thread>

namespace {

bool isHexDigit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// 16진 숫자만(1~maxDigits자) 받는다("0100A8ZZ"·"-1"·"0x…"·너무 긴 값은 거부). 주소 칸은 8자, 플래그 칸은 커널이 4자로 찍는다
std::optional<uint32_t> parseHex32(const std::string& text, std::size_t exactDigits = 0) {
    if (text.empty() || text.size() > 8) return std::nullopt;
    if (exactDigits != 0 && text.size() != exactDigits) return std::nullopt;
    uint32_t value = 0;
    for (char c : text) {
        if (!isHexDigit(c)) return std::nullopt;
        const int digit = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                                                                      : std::toupper(static_cast<unsigned char>(c)) - 'A' + 10;
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    return value;
}

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

std::vector<std::string> findDefaultGatewayIps(const std::string& routeText) {
    constexpr unsigned long kRouteFlagUp = 0x1;       // RTF_UP
    constexpr unsigned long kRouteFlagGateway = 0x2;  // RTF_GATEWAY
    std::vector<std::string> gateways;
    std::istringstream lines(routeText);
    std::string line;
    std::getline(lines, line);  // 머리글
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string iface, destination, gateway, flags, refCount, use, metric, mask;
        if (!(fields >> iface >> destination >> gateway >> flags >> refCount >> use >> metric >> mask)) continue;
        // 기본 경로는 목적지·마스크가 모두 0이다(VPN의 0.0.0.0/1 같은 반쪽 경로를 기본 경로로 오인하지 않게)
        if (destination != "00000000" || mask != "00000000") continue;
        const auto gatewayValue = parseHex32(gateway, 8);
        const auto flagValue = parseHex32(flags);
        if (!gatewayValue || !flagValue) continue;
        if ((*flagValue & kRouteFlagUp) == 0 || (*flagValue & kRouteFlagGateway) == 0) continue;
        // 커널은 in_addr 값(네트워크 바이트 순서)을 그대로 %08X로 찍으므로, 같은 기계에서 되돌리면 엔디언을 가정하지 않아도 된다
        in_addr address{};
        address.s_addr = *gatewayValue;
        char text[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &address, text, sizeof(text)) == nullptr) continue;
        gateways.emplace_back(text);
    }
    return gateways;
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

        // 라우터를 거쳐 온 접속은 ARP 표에 라우터 MAC으로 보인다. 그 MAC을 기기 ID로 쓰면 안 된다(기본 게이트웨이 전부)
        const auto routeText = readWholeFile(routePath_);
        if (routeText) {
            for (const auto& gatewayIp : MacResolverParse::findDefaultGatewayIps(*routeText)) {
                const auto gatewayMac = MacResolverParse::findArpMac(*arpText, gatewayIp);
                if (gatewayMac && *gatewayMac == *mac) return std::nullopt;
            }
        }
        return mac;
    }
    return std::nullopt;
}
