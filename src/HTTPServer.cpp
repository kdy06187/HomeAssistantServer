#include "HTTPServer.hpp"
#include "ApiAuth.hpp"
#include "DeviceManager.hpp"
#include "httplib.h"
#include "RequestValidation.hpp"
#include "json.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>

using json = nlohmann::json;

std::string controlMessageFor(const CommandResult& result) {
    if (result.outcome != CommandOutcome::Failed) return "명령 성공!";
    return result.error.code == ErrorCode::CommandBlocked ? "명령 성공!" : "명령 실패!";
}

namespace {
    // REST로 받을 수 있는 명령. PCDriver의 종료 메시지는 이 경로를 거치지 않아 영향이 없다.
    bool isAllowedControlCommand(const std::string& command) {
        return command == "ON" || command == "OFF";
    }

    // 느린 연결이 작업 풀을 오래 잡지 못하게 하는 상한. 읽기·쓰기 타임아웃은 소켓 대기 시간만 세므로
    // 핸들러가 오래 걸리는 요청(수동 새로고침 등)은 영향받지 않는다.
    constexpr time_t kSocketTimeoutSeconds = 5;
    constexpr time_t kKeepAliveTimeoutSeconds = 5;
    constexpr size_t kKeepAliveMaxRequests = 20;
    constexpr size_t kWorkerThreadCount = 8;

    void applyConnectionLimits(httplib::Server& server) {
        server.set_read_timeout(kSocketTimeoutSeconds);
        server.set_write_timeout(kSocketTimeoutSeconds);
        server.set_keep_alive_timeout(kKeepAliveTimeoutSeconds);
        server.set_keep_alive_max_count(kKeepAliveMaxRequests);
        // 고정 크기 풀. 기본 풀은 요청이 몰리면 스레드를 늘린다
        server.new_task_queue = [] { return new httplib::ThreadPool(kWorkerThreadCount); };
        // 기본값은 SO_REUSEPORT라 같은 포트를 두 프로세스가 함께 열 수 있다. SO_REUSEADDR만 켜서 재시작은 허용하고 중복 실행은 막는다
        server.set_socket_options([](socket_t sock) { httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1); });
    }

    // httplib는 바인딩 실패 원인(errno)을 알려 주지 않는다. 같은 조건으로 소켓을 한 번 직접 bind해 원인을 얻는다.
    // 0이면 진단 시점에는 bind가 성공했다는 뜻(그 사이 포트가 비었거나 경합).
    int diagnoseBindErrno(const std::string& address, int port) {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return errno;
        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        int result = 0;
        if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
            result = EINVAL;
        } else if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            result = errno;
        }
        ::close(fd);
        return result;
    }

    void logBindFailure(const std::string& address, int port) {
        const int cause = diagnoseBindErrno(address, port);
        std::cerr << "[HTTPServer] ❌ 바인딩 실패: 주소 " << address << ", 포트 " << port;
        if (cause != 0) {
            std::cerr << " (errno=" << cause << " " << std::strerror(cause) << ")" << std::endl;
        } else {
            std::cerr << " (진단 bind는 성공: 시작 직후 포트 경합 가능)" << std::endl;
        }
    }

    void respondInternalError(httplib::Response& res) {
        res.status = 500;
        res.set_content(R"({"error": "서버 내부 오류입니다."})", "application/json");
    }
}

Result<std::string> parseHttpBindAddress(const char* environmentValue) {
    if (environmentValue == nullptr || environmentValue[0] == '\0') {
        return Result<std::string>::success("0.0.0.0");
    }
    const std::string address(environmentValue);
    if (!RequestValidation::isValidIpv4Address(address)) {
        return Result<std::string>::failure(ErrorCode::InvalidRequest, "IPv4 형식이 아님");
    }
    return Result<std::string>::success(address);
}

HTTPServer::HTTPServer(DeviceManager& deviceManager, std::string apiToken, int port, std::string bindAddress)
    : mDeviceManager(deviceManager), mApiToken(std::move(apiToken)), mPort(port),
      mBindAddress(std::move(bindAddress)) {
}
HTTPServer::~HTTPServer() {
    stop();
}

bool HTTPServer::start(){
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    if (mServerThread.joinable()) {
        if (!mHasRunFinished) return false; // 이미 실행 중
        mServerThread.join();               // 바인딩 실패 등으로 끝난 이전 실행을 정리하고 다시 시작
    }
    mHasRunFinished = false;
    buildServer();
    try {
        mServerThread = std::thread(&HTTPServer::run, this);
    } catch (const std::system_error& e) {
        std::cerr << "[HTTPServer] ❌ 서버 스레드 생성 실패 (" << e.what() << ")" << std::endl;
        mHasRunFinished = true;
        return false;
    }
    std::cout << "[HTTPServer] HTTP 서버 시작 (주소: " << mBindAddress << ", 포트: " << mPort
              << ", 평문 HTTP)" << std::endl;
    return true;
}

void HTTPServer::stop(){
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    if (!mServerThread.joinable()) return; // 시작한 적이 없거나 이미 멈춤

    // run()이 bind·listen에 들어가기 전에 stop()이 불리면 무시되므로, run()이 끝날 때까지 반복 호출한다.
    while (!mHasRunFinished) {
        mServer->stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    mServerThread.join();
    std::cout << "[HTTPServer] HTTP 서버 종료" << std::endl;
}

bool HTTPServer::waitUntilReady(std::chrono::milliseconds timeout){
    if (!mServer) return false; // start()를 부르기 전
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!mServer->is_running()) {
        if (mHasRunFinished || std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void HTTPServer::buildServer(){
    mServer = std::make_unique<httplib::Server>();
    mServer->set_payload_max_length(16 * 1024);
    applyConnectionLimits(*mServer);
    registerRoutes();
}

void HTTPServer::registerRoutes(){
    addRoute(HttpMethod::Get, "/api/devices", [this](const httplib::Request&, httplib::Response& res) {
        std::string jsonResponse = this->handleGetDevices();
        res.set_content(jsonResponse, "application/json");
    });

    addRoute(HttpMethod::Post, "/api/control", [this](const httplib::Request& req, httplib::Response& res) {
        std::string jsonResponse = this->handleControlDevice(req.body);
        if (jsonResponse.find("\"error\"") != std::string::npos) {
            res.status = 400; 
        }
        res.set_content(jsonResponse, "application/json");
    });
    addRoute(HttpMethod::Post, "/api/commission", [this](const httplib::Request& req, httplib::Response& res){
        std::string jsonResponse = this->handleCommissionDevice(req.body);
        if (jsonResponse.find("\"error\"") != std::string::npos) {
            res.status = 400; 
        }
        res.set_content(jsonResponse, "application/json");
    });

    addRoute(HttpMethod::Post, "/api/devices/delete", [this](const httplib::Request& req, httplib::Response& res) {
        std::string jsonResponse = this->handleDeleteDevice(req.body);
        if (jsonResponse.find("\"error\"") != std::string::npos) {
            res.status = 400; 
        }
        res.set_content(jsonResponse, "application/json");
    });

    addRoute(HttpMethod::Get, R"(/api/devices/([^/]+)/state)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string deviceId = req.matches[1];
        bool isManualRefresh = false; // 기본값은 false
        if (req.has_param("isManualRefresh")) {
            std::string paramValue = req.get_param_value("isManualRefresh");
            // 안드로이드가 ?isManualRefresh=true 형태로 보냈을 때 처리
            if (paramValue == "true") { 
                isManualRefresh = true;
            }
        }
        std::string currentState = mDeviceManager.getDeviceState(deviceId, isManualRefresh);
        
        json response;
        response["deviceId"] = deviceId;
        response["state"] = currentState;
        
        res.set_content(response.dump(), "application/json");
    });
    addRoute(HttpMethod::Get, R"(/api/devices/([^/]+)/energy)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string deviceId = req.matches[1];
        std::string jsonResponse = this->handleGetDeviceEnergy(deviceId);
        if (jsonResponse.find("\"error\"") != std::string::npos) {
            res.status = 400;
        }
        res.set_content(jsonResponse, "application/json");
    });

    addRoute(HttpMethod::Get, R"(/api/devices/([^/]+)/energy/history)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string deviceId = req.matches[1];
        std::string result = this->handleGetDeviceEnergyHistory(deviceId);
        res.set_content(result, "application/json");
    });

    // 없는 경로도 토큰이 틀리면 401이어야 하므로, 마지막에 전체 매칭 라우트를 둔다(앞선 라우트가 먼저 매칭된다).
    const auto notFound = [](const httplib::Request&, httplib::Response& res) { res.status = 404; };
    for (HttpMethod method : {HttpMethod::Get, HttpMethod::Post, HttpMethod::Put,
                              HttpMethod::Delete, HttpMethod::Patch, HttpMethod::Options}) {
        addRoute(method, ".*", notFound);
    }
}

void HTTPServer::run(){
    httplib::Server& svr = *mServer;
    if (!svr.bind_to_port(mBindAddress, mPort)) {
        logBindFailure(mBindAddress, mPort);
    } else {
        std::cout << "[HTTPServer]네트워크 소켓 개방 완료" << std::endl;
        svr.listen_after_bind(); // stop()이 부르면 반환한다
    }
    mHasRunFinished = true;
}

void HTTPServer::addRoute(HttpMethod method, const std::string& pattern, RouteHandler handler) {
    auto guarded = [this, handler = std::move(handler)](const httplib::Request& req, httplib::Response& res) {
        if (!ApiAuth::isAuthorized(req.get_header_value("Authorization"), mApiToken)) {
            // 제시된 토큰은 일부라도 로그에 남기지 않는다.
            std::cerr << "[HTTPServer] 인증 실패: " << req.remote_addr << " " << req.method << " " << req.path << std::endl;
            res.status = 401;
            res.set_header("WWW-Authenticate", "Bearer");
            res.set_content(R"({"error": "인증이 필요합니다."})", "application/json");
            return;
        }
        // 핸들러가 처리하지 못한 예외는 내부 정보 없이 500으로 바꾼다(로그에도 메시지만 남긴다)
        try {
            handler(req, res);
        } catch (const std::exception& e) {
            std::cerr << "[HTTPServer] 처리 중 예외: " << req.method << " " << req.path << " (" << e.what() << ")" << std::endl;
            respondInternalError(res);
        } catch (...) {
            std::cerr << "[HTTPServer] 처리 중 알 수 없는 예외: " << req.method << " " << req.path << std::endl;
            respondInternalError(res);
        }
    };
    httplib::Server& svr = *mServer;
    switch (method) {
        case HttpMethod::Get:     svr.Get(pattern, guarded); break;
        case HttpMethod::Post:    svr.Post(pattern, guarded); break;
        case HttpMethod::Put:     svr.Put(pattern, guarded); break;
        case HttpMethod::Delete:  svr.Delete(pattern, guarded); break;
        case HttpMethod::Patch:   svr.Patch(pattern, guarded); break;
        case HttpMethod::Options: svr.Options(pattern, guarded); break;
    }
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
        deviceObj["state"] = device.state; // 현재 상태
        
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

            if (!isAllowedControlCommand(command)) {
                std::cerr << "[HTTPServer] 허용되지 않은 명령 거부: " << targetDeviceId << std::endl;
                return R"({"result": "error", "message": "허용되지 않은 명령입니다."})";
            }
            
            std::cout << "  ➔ 대상 기기: " << targetDeviceId << ", 명령: " << command << std::endl;

            // DeviceManager로 동적 명령 하달
            const CommandResult result = mDeviceManager.executeCommand(targetDeviceId, command);
            json response;
            response["status"] = result.state;
            response["message"] = controlMessageFor(result);
            return response.dump();
            
        } catch (const json::exception& e) {
            // JSON 형식이 잘못되었을 때 (앱 개발자의 실수 방어)
            std::cerr << "[HTTPServer] ❌ JSON 파싱 에러: " << e.what() << std::endl;
            return R"({"result": "error", "message": "잘못된 JSON 형식입니다."})";
        }
}

std::string HTTPServer::handleCommissionDevice(const std::string& requestBody){
    // Body에 Wi-Fi 비밀번호와 페어링 코드가 있으므로 내용을 출력하지 않는다.
    std::cout << "[HTTPServer] 기기 등록(POST) 요청 수신" << std::endl;
        
       try {
            // 들어온 문자열(req.body)을 JSON 객체로 파싱
            json j = json::parse(requestBody);
            
            // JSON에서 값을 추출
            std::string payload = j["payload"];
            std::string deviceName = j["deviceName"];
            std::string protocolTypeStr = j["protocolType"];

            std::string ssid = j.value("ssid", "");
            std::string password = j.value("password", "");

            std::vector<DriverType> driverTypes;
            if (j.contains("driverTypes") && j["driverTypes"].is_array()) {
                for (int typeInt : j["driverTypes"]) {
                    driverTypes.push_back(static_cast<DriverType>(typeInt));
                }
            }

            ProtocolType protocolType;
            if (protocolTypeStr == "MATTER") {
                protocolType = ProtocolType::MATTER;
            } else if (protocolTypeStr == "TCP_DIY") {
                protocolType = ProtocolType::TCP_DIY;
            } else {
                return R"({"result": "error", "message": "알 수 없는 프로토콜 타입입니다."})";
            }

            bool isSuccess = mDeviceManager.startCommissioning(protocolType, deviceName, payload, ssid, password, driverTypes);
            if (isSuccess) {
                return R"({"result": "success", "message": "기기 등록 성공!"})";
            } else {
                return R"({"result": "error", "message": "기기 등록 실패!"})";
            }
        } catch (const json::exception& e) {
            // JSON 형식이 잘못되었을 때 (앱 개발자의 실수 방어)
            // e.what()에는 입력 일부가 포함될 수 있어 오류 번호만 출력한다.
            std::cerr << "[HTTPServer] ❌ JSON 파싱 에러 (id: " << e.id << ")" << std::endl;
            return R"({"result": "error", "message": "잘못된 JSON 형식입니다."})";
        }
}

std::string HTTPServer::handleDeleteDevice(const std::string& requestBody) {
    std::cout << "[HTTPServer] 기기 삭제(POST) 요청 수신! Body: " << requestBody << std::endl;

    try {
        json j = json::parse(requestBody);
        std::string targetDeviceId = j["deviceId"];

        bool success = mDeviceManager.removeDevice(targetDeviceId);

        if (success) {
            return R"({"result": "success", "message": "기기 삭제 성공!"})";
        } else {
            return R"({"result": "error", "message": "기기 삭제 실패 또는 기기 없음"})";
        }
    } catch (const json::exception& e) {
        std::cerr << "[HTTPServer] ❌ JSON 파싱 에러: " << e.what() << std::endl;
        return R"({"result": "error", "message": "잘못된 JSON 형식입니다."})";
    }
}

std::string HTTPServer::handleGetDeviceEnergy(const std::string& deviceId) {
    std::cout << "[HTTPServer] 기기 전력량(실시간/누적) GET 요청 수신 Device ID: " << deviceId << std::endl;

    // 존재 확인을 먼저 하므로, 없는 기기는 DB에 아무것도 남기지 않는다
    const auto energy = mDeviceManager.readEnergy(deviceId);
    if (!energy.ok()) {
        if (energy.error.code == ErrorCode::DeviceNotFound) {
            return R"({"error": "등록되지 않은 기기입니다."})";
        }
        return R"({"error": "서버 내부 데이터 처리 오류입니다."})";
    }
    json response;
    response["device_status"] = energy.value->deviceState;
    response["activePower_mW"] = energy.value->activePower_mW;
    response["totalEnergy_mWh"] = energy.value->totalEnergy_mWh; // DB 보정이 끝난 진짜 데이터
    return response.dump();
}

std::string HTTPServer::handleGetDeviceEnergyHistory(const std::string& deviceId) {
    std::cout << "[HTTPServer] 시계열 데이터 GET 요청 수신 Device ID: " << deviceId << std::endl;

    // v1.0 계약: E7의 오류는 HTTP 200 + {"error"}. DB 오류도 빈 목록(성공)으로 숨기지 않는다
    const auto history = mDeviceManager.getEnergyHistory(deviceId);
    if (!history.ok()) {
        if (history.error.code == ErrorCode::DeviceNotFound) {
            return R"({"error": "등록되지 않은 기기입니다."})";
        }
        return R"({"error": "서버 내부 데이터 처리 오류입니다."})";
    }
    json response;
    response["device_status"] = history.value->deviceState;
    json logsArray = json::array();
    for (const auto& log : history.value->logs) {
        json item;
        item["timestamp"] = log.timestamp;
        item["totalEnergy_mWh"] = log.total_mwh;
        logsArray.push_back(item);
    }
    response["logs"] = logsArray;
    return response.dump();
}
