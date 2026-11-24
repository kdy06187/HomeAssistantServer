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
#include <memory>
#include <mutex>
#include <algorithm>

using json = nlohmann::json;

namespace {
// BLE 단계에서 이유가 정해진 실패(어댑터·기기 없음). 그 밖의 예외는 라이브러리 오류다
class CommissionAbort : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
// 서버 종료로 BLE 단계를 중간에 그만둠
class CommissionCancelled : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
constexpr int kBleScanRounds = 5;  // 1초씩 5번: 종료 요청을 1초 안에 알아채려고 5초 한 번 대신 나눠 스캔한다
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
                                                const std::unordered_set<std::string>& activeDeviceIds,
                                                const std::atomic<bool>& cancelRequested){
    if (cancelRequested) return Result<CommissionOutcome>::failure(ErrorCode::Cancelled, "등록 전 취소");
    const std::string& name = request.name;
    std::string server_ip = request.payload.empty() ? "192.168.219.109" : request.payload;
    int server_port = 8080;

    // 정리·스냅샷은 Wi-Fi 설정을 보내기 전에 한다. 보낸 뒤에 찍으면 빨리 붙은 새 기기가 "원래 있던 연결"로 들어가
    // 대기 내내 후보에서 빠진다. DB(앱 목록)에 없는 ID가 소켓을 차지하고 있으면 닫되 에이전트 연결은 지킨다
    std::unordered_set<std::string> keep = activeDeviceIds;
    if (server_.pcAgentId()) keep.insert(*server_.pcAgentId());
    const std::vector<std::string> existing_ids = server_.closeConnectionsNotIn(keep);

    // SimpleBLE는 어댑터·연결 오류를 예외로 알린다. 잡지 않으면 HTTP 스레드에서 std::terminate로 서버가 죽는다
    try {
        sendWifiConfigOverBle(request, server_ip, server_port, cancelRequested);
    } catch (const CommissionCancelled&) {
        std::cerr << "[TCPDriver] 서버 종료로 BLE 등록을 중단했습니다" << std::endl;
        return Result<CommissionOutcome>::failure(ErrorCode::Cancelled, "BLE 단계 취소");
    } catch (const CommissionAbort& abort) {
        return Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, abort.what());
    } catch (const std::exception& e) {
        std::cerr << "❌ [TCPDriver] BLE 처리 중 오류: " << RequestValidation::sanitizeForLog(e.what()) << std::endl;
        return Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "BLE 오류");
    }

    std::cout << "[TCPDriver] 와이파이 설정 전송 완료. TCP 접속 대기 중..." << std::endl;
    if (cancelRequested) {  // BLE 쓰기 중에 종료가 시작됐으면 대기 첫 1초도 쓰지 않는다
        std::cerr << "[TCPDriver] 서버 종료로 TCP 접속 대기를 중단했습니다" << std::endl;
        return Result<CommissionOutcome>::failure(ErrorCode::Cancelled, "접속 대기 취소");
    }

    for (int waited = 0; waited < kCommissionConnectWaitSeconds; ++waited) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (cancelRequested) {
            std::cerr << "[TCPDriver] 서버 종료로 TCP 접속 대기를 중단했습니다" << std::endl;
            return Result<CommissionOutcome>::failure(ErrorCode::Cancelled, "접속 대기 취소");
        }
        const auto candidate = TcpCommissionCandidate::pickNewDevice(existing_ids, server_.connectedIds(),
                                                                     activeDeviceIds, server_.pcAgentId());
        if (candidate) {
            // 수락 스레드가 이미 TCP_<MAC> ID로 연결을 맵에 넣었으므로 ID만 돌려주면 된다
            std::cout << "[TCPDriver] 커미셔닝 완료 : " << name << " (" << *candidate << ", " << (waited + 1) << "초)" << std::endl;
            return Result<CommissionOutcome>::success(CommissionOutcome{*candidate, server_.peerIpOf(*candidate).value_or("")});
        }
    }

    // 새 기기는 없고 등록된 기기가 새로 붙었다: 그 기기를 초기화해 다시 등록하려던 경우로 본다(연결은 그대로 둔다)
    const auto reconnected = TcpCommissionCandidate::findReconnectedActive(existing_ids, server_.connectedIds(), activeDeviceIds);
    if (reconnected) {
        std::cerr << "[TCPDriver] 새 기기 없음, 이미 등록된 기기가 다시 접속함 : " << *reconnected << std::endl;
        return Result<CommissionOutcome>::failure(ErrorCode::AlreadyRegistered, "등록된 기기 재접속");
    }
    std::cerr << "❌ [TCPDriver] 타임아웃: 기기가 와이파이에 접속하지 못했거나 서버에 연결하지 못했습니다." << std::endl;
    return Result<CommissionOutcome>::failure(ErrorCode::CommissionTimeout, "기기 접속 타임아웃");
}

void TCPDriver::sendWifiConfigOverBle(const CommissionRequest& request, const std::string& serverIp, int serverPort,
                                      const std::atomic<bool>& cancelRequested) {
    std::cout << "[TCPDriver] BLE 스캔 시작... " << std::endl;

    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        std::cerr << "[TCPDriver] 블루투스 어댑터를 찾을 수 없습니다." << std::endl;
        throw CommissionAbort("블루투스 어댑터 없음");
    }
    auto adapter = adapters[0];

    // 스캔 콜백은 SimpleBLE 스레드에서 불릴 수 있다. 결과는 락으로 지키고, 이 함수가 끝난 뒤 늦게 불려도
    // 사라진 지역 변수를 쓰지 않게 shared_ptr로 콜백과 함께 소유한다
    struct ScanResult {
        std::mutex mutex;
        bool found = false;
        SimpleBLE::Peripheral device;
    };
    auto scan = std::make_shared<ScanResult>();
    adapter.set_callback_on_scan_found([scan](SimpleBLE::Peripheral peripheral) {
        if (peripheral.identifier() != "TCPDevice_DIY") return;
        std::lock_guard<std::mutex> lock(scan->mutex);
        scan->device = peripheral;
        scan->found = true;
    });

    bool found = false;
    SimpleBLE::Peripheral target_device;
    {
        // 스캔이 어떻게 끝나든(찾음·못 찾음·취소·라이브러리 예외) 콜백을 빈 것으로 바꿔 더 받지 않는다
        struct ScanCallbackReset {
            SimpleBLE::Adapter& adapter;
            ~ScanCallbackReset() {
                try {
                    adapter.set_callback_on_scan_found([](SimpleBLE::Peripheral) {});
                } catch (...) {  // 소멸자에서 예외를 내보내지 않는다(원래 오류를 가리지 않게)
                }
            }
        } resetCallback{adapter};
        for (int round = 0; round < kBleScanRounds && !found; ++round) {
            if (cancelRequested) throw CommissionCancelled("BLE 스캔 중 취소");
            adapter.scan_for(1000);
            // 스캔 1초 사이에 종료가 시작됐으면 찾았어도 연결하지 않는다
            if (cancelRequested) throw CommissionCancelled("BLE 스캔 중 취소");
            std::lock_guard<std::mutex> lock(scan->mutex);
            found = scan->found;
            target_device = scan->device;
        }
    }
    if (!found) {
        std::cerr << "❌ [TCPDriver] TCPDevice_DIY 기기를 찾지 못했습니다." << std::endl;
        throw CommissionAbort("BLE 기기 없음");
    }

    std::cout << "[TCPDriver] 기기 발견! 연결 시도 중..." << std::endl;
    target_device.connect();
    try {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        if (cancelRequested) throw CommissionCancelled("BLE 연결 뒤 취소");  // 아래 catch가 연결을 끊는다

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
    // main·정지 가드·소멸자가 차례로 부르므로 실제로 멈춘 첫 번째만 로그를 남긴다
    const bool wasRunning = server_.boundPort() != 0;
    server_.stop();
    if (wasRunning) std::cout << "[TCPDriver] TCP 서버 종료" << std::endl;
}

void TCPDriver::finalizeCommission(const CommissionOutcome& outcome, bool accepted) {
    // 서버가 등록하지 않은 기기는 RESET으로 BLE 설정 모드로 돌려보내고 연결을 닫는다
    if (!accepted) unpairDevice(outcome.deviceId);
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
