#include "TCPDriver.hpp"
#include <iostream>
#include <vector>
#include <simpleble/SimpleBLE.h>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include "json.hpp"
#include "TcpProtocol.hpp"
#include "TcpCommissionCandidate.hpp"
#include "RequestValidation.hpp"
#include <stdexcept>
#include <algorithm>

using json = nlohmann::json;

namespace {
// BLE 단계에서 이유가 정해진 실패(어댑터·기기 없음). 그 밖의 예외는 라이브러리 오류다
class CommissionAbort : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
}  // namespace
TCPDriver::TCPDriver(MacResolver& macResolver, std::optional<std::string> pcAgentId)
    : server_(macResolver, std::move(pcAgentId)) {
    std::cout << "[TCPDriver] 인스턴스 생성" << std::endl;
}

TCPDriver::~TCPDriver(){
    stopServer();
    std::cout << "[TCPDriver] 인스턴스 소멸" << std::endl;
}

bool TCPDriver::sendCommand(const std::string& device_id, const std::string& command){
    return messageSender_.sendTo(device_id, command).ok();
}
Result<CommissionOutcome> TCPDriver::commission(const CommissionRequest& request,
                                                const std::unordered_set<std::string>& activeDeviceIds){
    const std::string& name = request.name;
    std::string server_ip = request.payload.empty() ? "192.168.219.109" : request.payload;
    int server_port = 8080;

    // SimpleBLE는 어댑터·연결 오류를 예외로 알린다. 잡지 않으면 HTTP 스레드에서 std::terminate로 서버가 죽는다
    try {
        sendWifiConfigOverBle(request, server_ip, server_port);
    } catch (const CommissionAbort& abort) {
        return Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, abort.what());
    } catch (const std::exception& e) {
        std::cerr << "❌ [TCPDriver] BLE 처리 중 오류: " << RequestValidation::sanitizeForLog(e.what()) << std::endl;
        return Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "BLE 오류");
    }

    std::cout << "[TCPDriver] 와이파이 설정 전송 완료. TCP 접속 대기 중..." << std::endl;
    // 소켓 목록과 DB 동기화: DB(앱 목록)에 없는 ID가 소켓을 차지하고 있으면 닫는다. 에이전트 연결은 지킨다
    std::unordered_set<std::string> keep = activeDeviceIds;
    if (server_.pcAgentId()) keep.insert(*server_.pcAgentId());
    server_.closeConnectionsNotIn(keep);
    const std::vector<std::string> existing_ids = server_.connectedIds();

    for (int waited = 0; waited < kCommissionConnectWaitSeconds; ++waited) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto candidate = TcpCommissionCandidate::pickNewDevice(existing_ids, server_.connectedIds(),
                                                                     activeDeviceIds, server_.pcAgentId());
        if (candidate) {
            // 수락 스레드가 이미 TCP_<MAC> ID로 연결을 맵에 넣었으므로 ID만 돌려주면 된다
            std::cout << "[TCPDriver] 커미셔닝 완료 : " << name << " (" << *candidate << ", " << (waited + 1) << "초)" << std::endl;
            return Result<CommissionOutcome>::success(CommissionOutcome{*candidate});
        }
    }

    std::cerr << "❌ [TCPDriver] 타임아웃: 기기가 와이파이에 접속하지 못했거나 서버에 연결하지 못했습니다." << std::endl;
    return Result<CommissionOutcome>::failure(ErrorCode::CommissionTimeout, "기기 접속 타임아웃");
}

void TCPDriver::sendWifiConfigOverBle(const CommissionRequest& request, const std::string& serverIp, int serverPort) {
    std::cout << "[TCPDriver] BLE 스캔 시작... " << std::endl;

    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        std::cerr << "[TCPDriver] 블루투스 어댑터를 찾을 수 없습니다." << std::endl;
        throw CommissionAbort("블루투스 어댑터 없음");
    }
    auto adapter = adapters[0];

    bool found = false;
    SimpleBLE::Peripheral target_device;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral peripheral) {
        if (peripheral.identifier() == "TCPDevice_DIY") {
            target_device = peripheral;
            found = true;
        }
    });

    adapter.scan_for(5000);

    if (!found) {
        std::cerr << "❌ [TCPDriver] TCPDevice_DIY 기기를 찾지 못했습니다." << std::endl;
        throw CommissionAbort("BLE 기기 없음");
    }

    std::cout << "[TCPDriver] 기기 발견! 연결 시도 중..." << std::endl;
    target_device.connect();
    try {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        json configJson;
        configJson["ssid"] = request.ssid;
        configJson["pw"] = request.password;
        configJson["serverIp"] = serverIp;
        configJson["serverPort"] = serverPort;
        std::string configStr = configJson.dump();

        target_device.write_request("19b10000-e8f2-537e-4f6c-d104768a1214",
                                    "19b10001-e8f2-537e-4f6c-d104768a1214",
                                    configStr);
    } catch (...) {
        // 연결한 뒤 실패하면 BLE 연결을 남기지 않는다(끊기 실패는 원래 오류를 가리지 않게 삼킨다)
        try {
            target_device.disconnect();
        } catch (...) {
        }
        throw;
    }
    target_device.disconnect();
}

bool TCPDriver::startServer(int port, const std::string& bindAddress){
    if (!server_.start(port, bindAddress)) {
        std::cerr << "[TCPDriver] TCP 서버 시작 실패 ( 주소 : " << bindAddress << ", 포트 : " << port << ")" << std::endl;
        return false;
    }
    std::cout << "[TCPDriver] TCP 서버 시작 ( 주소 : " << bindAddress << ", 포트 : " << port << ", 평문)" << std::endl;
    return true;
}

void TCPDriver::stopServer(){
    server_.stop();
    std::cout << "[TCPDriver] TCP 서버 종료" << std::endl;
}

bool TCPDriver::unpairDevice(const std::string& deviceId){
    // 맵에서 먼저 빼고(맵 락), 락을 푼 뒤 그 연결로만 RESET을 보낸다
    auto connection = server_.take(deviceId);
    if (!connection) return false;
    connection->sendOnly(TcpProtocol::resetLine());
    connection->shutdown();
    std::cout << "[TCPDriver] 아두이노 연결 해제 완료: " << deviceId << std::endl;
    return true;
}
std::string TCPDriver::readDeviceState(const std::string& deviceId, bool /*isManualRequest*/){
    // 상태 요청 패킷 생성
    json res = server_.sendAndReceive(deviceId, TcpProtocol::buildGetStateRequest());
    // state가 없거나 문자열이 아니면(숫자 등) 기존 "응답 오류" 경로와 같게 처리한다
    const auto parsedState = TcpProtocol::parseStateResponse(res);
    if (!parsedState) {
        std::cerr << "[TCPDriver] 기기 상태 조회 실패 (오프라인 또는 응답 오류): " << deviceId << std::endl;
        return "OFFLINE";
    }
    const std::string& currentState = *parsedState;
    std::cout << "[TCPDriver] 기기 상태 조회 : " << deviceId << " 현재 상태 ➔ [" << currentState << "]" << std::endl;

    return currentState;
}
Status TCPDriver::sendTo(const std::string& deviceId, const std::string& jsonLine) {
    return messageSender_.sendTo(deviceId, jsonLine);
}

void TCPDriver::sendToPcAgent(const std::string& jsonLine) {
    messageSender_.sendToPcAgent(jsonLine);
}
