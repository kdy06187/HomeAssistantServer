#include "TCPDriver.hpp"
#include "DeviceManager.hpp"
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

using json = nlohmann::json;
TCPDriver::TCPDriver(DeviceManager& deviceManager) : 
    ProtocolDriver(deviceManager), 
    running_(false) {
    std::cout << "[TCPDriver] 인스턴스 생성" << std::endl;
}

TCPDriver::~TCPDriver(){
    stopServer();
    std::cout << "[TCPDriver] 인스턴스 소멸" << std::endl;
}

bool TCPDriver::sendCommand(std::string device_id, std::string command){
    // JSON 명령 패킷 생성
    json res;
    res["action"] = "CONTROL";
    res["command"] = command; // "ON" 또는 "OFF"
    
    res = sendAndReceive(device_id, res);
    if (res.is_null()) {
        std::cerr << "[TCPDriver] 명령 전송 실패 (기기 오프라인): " << device_id << std::endl;
        return false;
    }
    std::cout << "[TCPDriver] 명령 전송 성공: " << device_id << " -> " << command << std::endl;

    return true;
}
bool TCPDriver::commissionDevice(std::string name, std::string payload, const std::string& ssid, const std::string& password){
    std::string server_ip = "192.168.219.108"; 
    int server_port = 8080;

    std::cout << "[TCPDriver] BLE 스캔 시작... " << std::endl;

    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        std::cerr << "[TCPDriver] 블루투스 어댑터를 찾을 수 없습니다." << std::endl;
        return false;
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
        return false;
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

    size_t initial_socket_count = 0;
    {
        std::lock_guard<std::mutex> lock(sockets_mutex_);
        initial_socket_count = client_sockets_.size();
    }
    std::string registered_device_id = "";
    for (int i = 0; i < 15; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        std::lock_guard<std::mutex> lock(sockets_mutex_);
        if (client_sockets_.size() > initial_socket_count) {
            // 가장 최근에 들어온(혹은 기존에 없던) 기기 ID를 찾아냅니다.
            for (auto const& [id, socket_fd] : client_sockets_) {
                // 이전에 없던 새로운 ID인지 확인 (혹은 단순하게 가장 최근 등록된 녀석 획득)
                // 여기서는 간단하게 새롭게 매핑된 ID를 탐색합니다.
                registered_device_id = id;
                break; 
            }
            break;
        }
    }
    if (!registered_device_id.empty()) {
        // acceptLoop에서 이미 "ARD_IP" 형태로 ID와 소켓이 맵핑되었으므로 바로 디바이스 매니저에 등록
        mDeviceManager.addDevice(registered_device_id, name, ProtocolType::TCP_DIY);
        std::cout << "[TCPDriver] 커미셔닝 완료 : " << name << " (" << registered_device_id << ")" << std::endl;
        return true;
    }
    
    std::cerr << "❌ [TCPDriver] 타임아웃: 기기가 와이파이에 접속하지 못했거나 서버에 연결하지 못했습니다." << std::endl;
    return false;
}

void TCPDriver::startServer(int port){
    if(running_) return; // 이미 서버가 실행 중이면 무시
    running_ = true;
    // 서버를 별도의 스레드에서 실행
    server_thread_ = std::thread(&TCPDriver::accecptLoop, this,port);
    std::cout << "[TCPDriver] TCP 서버 시작 ( 포트 : " << port << ")" << std::endl;
}

void TCPDriver::stopServer(){
    running_ = false;
    if(server_thread_.joinable()){
        server_thread_.join();
    }
    std::cout << "[TCPDriver] TCP 서버 종료" << std::endl;
}

void TCPDriver::accecptLoop(int port){
    // TCP 서버 소켓 생성
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0){
        std::cerr << "[TCPDriver] 에러 : 서버 소켓 생성 실패" << std::endl;
        return;
    }
    // 포트 충돌 방지 옵션
    int opt = 1;
    setsockopt(server_fd,SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    //bind 포트와 소켓 결합
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // 모든 IP의 접속 허용
    address.sin_port = htons(port); // 포트 번호 지정

    if(bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0){
        std::cerr << "[TCPDriver] 에러 : 포트 " << port << " 바인딩 실패" << std::endl;
        close(server_fd);
        return;
    }
    //listen (대기열 생성)
    if(listen(server_fd,10) < 0){
        std::cerr << "[TCPDriver] 에러 : 포트 " << port << " 리슨 실패" << std::endl;
        close(server_fd);
        return;
    }

    std::cout << "[TCPDriver] " << port << " 포트에서 클라이언트 연결 대기 중..." << std::endl;
    while(running_){
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        //기기 접속 대기
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen); // 클라이언트 연결 수락 - 전용 1:1 통신 채널 파이프 
        if(client_fd < 0){
            std::cerr << "[TCPDriver] 에러 : 클라이언트 연결 수락 실패" << std::endl;
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
        std::string client_ip(ip_str);

        std::cout << "[TCPDriver] 클라이언트 연결 수락 IP : " << client_ip << std::endl;
        
        // Json 형식으로 기기 ID를 받지만 임시로 지정
        std::string device_id = "ARD_" + client_ip;
        {
            std::lock_guard<std::mutex> lock(sockets_mutex_);
            client_sockets_[device_id] = client_fd; // 기기 ID와 소켓 번호 저장
        }
        std::cout << "[TCPDriver] 기기 ID [" << device_id << "] 와 소켓이 매핑되었습니다." << std::endl;
    }
    close(server_fd);
}

bool TCPDriver::unpairDevice(std::string deviceId){
    std::lock_guard<std::mutex> lock(sockets_mutex_);
    auto it = client_sockets_.find(deviceId);
    if(it != client_sockets_.end()){
        int client_socket = it->second;
        close(client_socket);
        client_sockets_.erase(it);
        std::cout << "[TCPDriver] 아두이노 연결 해제 완료: " << deviceId << std::endl;
        return true;
    }
    return false;
}
std::string TCPDriver::readDeviceState(std::string deviceId, bool isManualRequest){
    // 상태 요청 패킷 생성
    json req;
    req["action"] = "GET_STATE";


    json res = sendAndReceive(deviceId, req);
    if (res.is_null() || !res.contains("state")) {
        std::cerr << "[TCPDriver] 기기 상태 조회 실패 (오프라인 또는 응답 오류): " << deviceId << std::endl;
        return "UNKNOWN";
    }
    std::string currentState = res["state"].get<std::string>();
    std::cout << "[TCPDriver] 기기 상태 조회 : " << deviceId << " 현재 상태 ➔ [" << currentState << "]" << std::endl;

    return currentState;
}
json TCPDriver::sendAndReceive(const std::string& deviceId, const json& requestJson) {
    int socket_fd = -1;
    {
        std::lock_guard<std::mutex> lock(sockets_mutex_);
        auto it = client_sockets_.find(deviceId);
        if (it == client_sockets_.end()) {
            return nullptr;
        }
        socket_fd = it->second;
    }

    auto cleanupSocket = [&]() -> nlohmann::json {
        std::lock_guard<std::mutex> lock(sockets_mutex_);
        client_sockets_.erase(deviceId);
        close(socket_fd);
        return nullptr;
    };

    std::string packet = requestJson.dump() + "\n";
    if (send(socket_fd, packet.c_str(), packet.length(), 0) < 0) {
        return cleanupSocket();
    }

    struct timeval tv;
    tv.tv_sec = 1; // 1초 타임아웃
    tv.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    char buffer[512] = {0};
    ssize_t read_bytes = recv(socket_fd, buffer, sizeof(buffer) - 1, 0);
    if (read_bytes <= 0) {
        std::cerr << "[TCPDriver] 응답 없음 : " << deviceId << std::endl;
        return cleanupSocket();
    }


    json lastValidJson = nullptr;
    std::string rawData(buffer);
    std::istringstream stream(rawData);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line == "\r") continue;
        try {
            lastValidJson = json::parse(line);
        } catch (...) {
            std::cerr << "[TCPDriver] JSON 파싱 에러 (응답: " << line << ")" << std::endl;
        }
    }
    return lastValidJson;
      
}