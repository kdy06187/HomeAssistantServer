#include "HTTPServer.hpp"
#include "DeviceManager.hpp"
#include "httplib.h"
#include "json.hpp"
#include <iostream>

using json = nlohmann::json;

HTTPServer::HTTPServer(DeviceManager& deviceManager, int port)
    : mDeviceManager(deviceManager), mPort(port), mIsRunning(false) {
}
HTTPServer::~HTTPServer() {
    stop();
}

bool HTTPServer::start(){
    if(mIsRunning) return false; // 이미 서버가 실행 중이면 무시
    mIsRunning = true;
    mServerThread = std::thread(&HTTPServer::run, this);
    std::cout << "[HTTPServer] HTTP 서버 시작 (포트: " << mPort << ")" << std::endl;
    return true;
}

void HTTPServer::stop(){
    if (!mIsRunning) return;

    mIsRunning = false;
    if(mServerThread.joinable()){
        mServerThread.join();
    }
    std::cout << "[HTTPServer] HTTP 서버 종료" << std::endl;
}

void HTTPServer::run(){
    httplib::Server svr;
    
    svr.Get("/api/devices", [this](const httplib::Request&, httplib::Response& res) {
        std::string jsonResponse = this->handleGetDevices();
        res.set_content(jsonResponse, "application/json");
    });

    svr.Post("/api/control", [this](const httplib::Request& req, httplib::Response& res) {
        std::string jsonResponse = this->handleControlDevice(req.body);
        if (jsonResponse.find("\"error\"") != std::string::npos) {
            res.status = 400; 
        }
        res.set_content(jsonResponse, "application/json");
    });
    std::cout << "[HTTPServer]네트워크 소켓 개방 완료" << std::endl;
    svr.listen("0.0.0.0", mPort);
}

std::string HTTPServer::handleGetDevices(){
    std::cout << "[HTTPServer] 기기 목록 조회(GET) 요청 수신" << std::endl;
    json jsonArray = json::array();
    std::vector<Device> deviceList = mDeviceManager.getAllDevices();
    for (const auto& device : deviceList) {
        json deviceObj;
        
        // 주의: Device 클래스의 멤버 변수 이름(id, name 등)이 private이면 
        // device.getId() 같은 getter 메서드로 바꿔 적어주세요!
        deviceObj["id"] = device.id; 
        deviceObj["name"] = device.name;
        deviceObj["type"] = mDeviceManager.getProtocolString(device.protocol_type); // enum 이름 확인!
        
        jsonArray.push_back(deviceObj);
    }
    json response;
    response["status"] = "success";
    response["total_count"] = deviceList.size();
    response["devices"] = jsonArray;
    return response.dump();
}

std::string HTTPServer::handleControlDevice(const std::string& requestBody){
    std::cout << "[HTTPServer] 제어(POST) 요청 수신! Body: " << requestBody << std::endl;
        
       try {
            // 들어온 문자열(req.body)을 JSON 객체로 파싱
            json j = json::parse(requestBody);
            
            // JSON에서 값을 추출
            std::string targetDeviceId = j["deviceId"];
            std::string command = j["command"];
            
            std::cout << "  ➔ 대상 기기: " << targetDeviceId << ", 명령: " << command << std::endl;

            // DeviceManager로 동적 명령 하달
            bool success = mDeviceManager.executeCommand(targetDeviceId, command);

            if (success) {
                return R"({"result": "success", "message": "명령 성공!"})";
            } else {
                return R"({"result": "error", "message": "명령 실패 또는 기기 오프라인"})";
            }
            
        } catch (const json::exception& e) {
            // JSON 형식이 잘못되었을 때 (앱 개발자의 실수 방어)
            std::cerr << "[HTTPServer] ❌ JSON 파싱 에러: " << e.what() << std::endl;
            return R"({"result": "error", "message": "잘못된 JSON 형식입니다."})";
        }
}