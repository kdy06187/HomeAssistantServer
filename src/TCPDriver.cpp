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
#include <algorithm>

using json = nlohmann::json;
TCPDriver::TCPDriver() {
    std::cout << "[TCPDriver] 인스턴스 생성" << std::endl;
}

TCPDriver::~TCPDriver(){
    stopServer();
    std::cout << "[TCPDriver] 인스턴스 소멸" << std::endl;
}

bool TCPDriver::sendCommand(const std::string& device_id, const std::string& command){
    // '{'로 시작하면 JSON 그대로, "ON"/"OFF" 같은 단순 명령은 릴레이 제어 패킷으로 감싼다(하위 호환)
    json requestJson;
    if (!TcpProtocol::buildCommandRequest(command, requestJson)) {
        std::cerr << "[TCPDriver] 명령어 JSON 파싱 실패" << std::endl;
        return false;
    }

    json responseJson = server_.sendAndReceive(device_id, requestJson);

    if (responseJson.is_null()) {
        std::cerr << "[TCPDriver] 명령 전송 실패 (기기 오프라인): " << device_id << std::endl;
        return false;
    }
    // 응답 패킷에 "status" 필드가 "success"인지 확인
    if (responseJson.contains("status") && responseJson["status"] == "success") {
        std::cout << "[TCPDriver] 명령 전송 성공: " << device_id << std::endl;
        return true;
    }
    // 에러 상태이거나 규격에 맞지 않는 응답인 경우
    std::string error_msg = responseJson.contains("message") ? responseJson["message"].get<std::string>() : "알 수 없는 에러";
    std::cerr << "[TCPDriver] 기기에서 에러 반환: " << error_msg << std::endl;
    return false;
}
Result<CommissionOutcome> TCPDriver::commission(const CommissionRequest& request,
                                                const std::unordered_set<std::string>& activeDeviceIds){
    const std::string& name = request.name;
    const std::string& ssid = request.ssid;
    const std::string& password = request.password;
    std::string server_ip = request.payload.empty() ? "192.168.219.109" : request.payload;
    int server_port = 8080;

    std::cout << "[TCPDriver] BLE 스캔 시작... " << std::endl;

    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        std::cerr << "[TCPDriver] 블루투스 어댑터를 찾을 수 없습니다." << std::endl;
        return Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "블루투스 어댑터 없음");
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
        return Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "BLE 기기 없음");
    }

    std::cout << "[TCPDriver] 기기 발견! 연결 시도 중..." << std::endl;
    target_device.connect();

    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    
    json configJson;
    configJson["ssid"] = ssid;
    configJson["pw"] = password;
    configJson["serverIp"] = server_ip;
    configJson["serverPort"] = server_port;
    std::string configStr = configJson.dump();

    target_device.write_request("19b10000-e8f2-537e-4f6c-d104768a1214", 
                                "19b10001-e8f2-537e-4f6c-d104768a1214", 
                                configStr);
    target_device.disconnect();

    std::cout << "[TCPDriver] 와이파이 설정 전송 완료. TCP 접속 대기 중..." << std::endl;
    // 소켓 목록과 DB 동기화: DB(앱 목록)에 없는 ID가 소켓을 차지하고 있으면 닫는다
    server_.closeConnectionsNotIn(activeDeviceIds);
    // 현재 연결된 ID 목록 저장
    const std::vector<std::string> existing_ids = server_.connectedIds();

    std::string registered_device_id = "";
    for (int i = 0; i < 15; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        for (const auto& id : server_.connectedIds()) {
            // 현재 소켓 목록의 ID가 기존 목록(existing_ids)에 없다면 = '새로 접속한 기기'
            if (std::find(existing_ids.begin(), existing_ids.end(), id) == existing_ids.end()) {
                registered_device_id = id;
                break;
            }
        }
        if (!registered_device_id.empty()) {
            break;
        }
    }
    if (!registered_device_id.empty()) {
        // acceptLoop에서 이미 "ARD_IP" 형태로 ID와 소켓이 맵핑되었으므로 ID만 돌려주면 된다
        std::cout << "[TCPDriver] 커미셔닝 완료 : " << name << " (" << registered_device_id << ")" << std::endl;
        return Result<CommissionOutcome>::success(CommissionOutcome{registered_device_id});
    }
    
    std::cerr << "❌ [TCPDriver] 타임아웃: 기기가 와이파이에 접속하지 못했거나 서버에 연결하지 못했습니다." << std::endl;
    return Result<CommissionOutcome>::failure(ErrorCode::CommissionTimeout, "기기 접속 타임아웃");
}

void TCPDriver::startServer(int port){
    if (!server_.start(port)) {
        std::cerr << "[TCPDriver] TCP 서버 시작 실패 ( 포트 : " << port << ")" << std::endl;
        return;
    }
    std::cout << "[TCPDriver] TCP 서버 시작 ( 포트 : " << port << ")" << std::endl;
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
std::string TCPDriver::readDeviceState(const std::string& deviceId, bool isManualRequest){
    // 상태 요청 패킷 생성
    json res = server_.sendAndReceive(deviceId, TcpProtocol::buildGetStateRequest());
    if (res.is_null() || !res.contains("state")) {
        std::cerr << "[TCPDriver] 기기 상태 조회 실패 (오프라인 또는 응답 오류): " << deviceId << std::endl;
        return "OFFLINE";
    }
    std::string currentState = res["state"].get<std::string>();
    std::cout << "[TCPDriver] 기기 상태 조회 : " << deviceId << " 현재 상태 ➔ [" << currentState << "]" << std::endl;

    return currentState;
}
void TCPDriver::SendCommandTo(const std::string& target_id, const std::string& payload) {
    if (target_id == "ALL") {
        // 연결 목록 사본을 순회한다. 순회 중 삭제·재접속이 있어도 맵을 직접 돌지 않으므로 안전하다
        for (const auto& id : server_.connectedIds()) {
            this->sendCommand(id, payload);
        }
        return;
    }
    this->sendCommand(target_id, payload);
}