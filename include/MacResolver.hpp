#pragma once
#include <chrono>
#include <optional>
#include <string>

// 접속한 기기의 IPv4 주소로 MAC 주소를 찾는다. DIY 기기는 접속 때 ID를 보내지 않으므로,
// IP처럼 바뀌는 값 대신 MAC으로 기기 ID(TCP_<MAC12>)를 만든다.
class MacResolver {
public:
    virtual ~MacResolver() = default;
    // "AABBCCDDEEFF"(대문자 12자) 또는 없음(조회 실패·라우터 너머 접속)
    virtual std::optional<std::string> resolve(const std::string& peerIpv4) = 0;
};

// MAC 12자(대문자)로 TCP 기기 ID를 만든다
inline std::string makeTcpDeviceId(const std::string& normalizedMac) { return "TCP_" + normalizedMac; }

// /proc/net 파일 내용을 해석하는 순수 함수(파일을 읽지 않는다)
namespace MacResolverParse {
// "AA:BB:CC:DD:EE:FF"·"aa-bb-…"·"aabbccddeeff" → "AABBCCDDEEFF". 구분자는 한 종류로 2자마다여야 한다
std::optional<std::string> normalizeMac(const std::string& text);
// /proc/net/arp 내용에서 ip와 완전히 같은 줄의 MAC. 미완성 항목(플래그 0x0, MAC 0)은 없음으로 본다
std::optional<std::string> findArpMac(const std::string& arpText, const std::string& ipv4);
// /proc/net/route 내용에서 기본 경로(목적지 0, 게이트웨이 플래그)의 게이트웨이 IPv4(점 표기)
std::optional<std::string> findDefaultGatewayIp(const std::string& routeText);
}  // namespace MacResolverParse

// 커널 ARP 표(/proc/net/arp)를 읽는 실제 구현. 파일 읽기만 하고 패킷을 보내지 않는다.
// 접속 IP의 MAC이 기본 게이트웨이의 MAC과 같으면 라우터 너머에서 온 접속이라 없음으로 본다.
class ProcNetArpResolver : public MacResolver {
public:
    static constexpr int kDefaultAttempts = 3;
    static constexpr std::chrono::milliseconds kDefaultRetryInterval{100};

    explicit ProcNetArpResolver(std::string arpPath = "/proc/net/arp", std::string routePath = "/proc/net/route",
                                int attempts = kDefaultAttempts,
                                std::chrono::milliseconds retryInterval = kDefaultRetryInterval);
    std::optional<std::string> resolve(const std::string& peerIpv4) override;

private:
    const std::string arpPath_;
    const std::string routePath_;
    const int attempts_;
    const std::chrono::milliseconds retryInterval_;
};
