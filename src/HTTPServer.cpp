#include "HTTPServer.hpp"
#include "ApiAuth.hpp"
#include "DeviceManager.hpp"
#include "httplib.h"
#include "ErrorMapping.hpp"
#include "RequestValidation.hpp"
#include "json.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <sys/socket.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>

using json = nlohmann::json;

namespace {
    // REST로 받을 수 있는 명령. PCDriver의 종료 메시지는 이 경로를 거치지 않아 영향이 없다.
    bool isAllowedControlCommand(const std::string& command) {
        return command == "ON" || command == "OFF";
    }

    // 기기 호출이 서버 종료로 취소되면 앱에는 "기기 응답 없음"(502)으로 알린다.
    // Cancelled의 기본 매핑(200)은 등록 작업 조회 본문 안에 넣는 용도라, 그대로 쓰면 성공 상태 코드로 나간다
    ErrorCode deviceCallErrorCode(ErrorCode code) {
        return code == ErrorCode::Cancelled ? ErrorCode::DeviceUnreachable : code;
    }

    // 명령 결과를 응답으로 바꾼다. 성공 200, 지연 수락 202(state는 바뀌기 전 상태), 실패는 오류 코드대로
    ApiResponse controlResponseFor(const CommandResult& result, const std::string& deviceId, const std::string& command) {
        switch (result.outcome) {
            case CommandOutcome::Succeeded:
                return ApiResponse::ok(json{{"deviceId", deviceId}, {"state", result.state}});
            case CommandOutcome::Accepted:
                return ApiResponse::ok(json{{"deviceId", deviceId}, {"state", result.state},
                                            {"pendingCommand", command}, {"delaySeconds", result.delaySeconds}}, 202);
            case CommandOutcome::Failed:
                break;
        }
        return ApiResponse::error(deviceCallErrorCode(result.error.code));
    }

    // 느린 연결이 작업 풀을 오래 잡지 못하게 하는 상한. 읽기·쓰기 타임아웃은 소켓 대기 시간만 세므로
    // 핸들러가 오래 걸리는 요청(수동 새로고침 등)은 영향받지 않는다.
    constexpr time_t kSocketTimeoutSeconds = 5;
    constexpr time_t kKeepAliveTimeoutSeconds = 5;
    constexpr size_t kKeepAliveMaxRequests = 20;
    constexpr size_t kWorkerThreadCount = 8;
    // ErrorCode에 없는 httplib 전용 오류라 ErrorMapping을 거치지 않는다
    constexpr const char* kPayloadTooLargeBody =
        R"({"error":{"code":"PAYLOAD_TOO_LARGE","message":"요청 본문이 너무 큽니다."}})";

    void applyConnectionLimits(httplib::Server& server) {
        server.set_read_timeout(kSocketTimeoutSeconds);
        server.set_write_timeout(kSocketTimeoutSeconds);
        server.set_keep_alive_timeout(kKeepAliveTimeoutSeconds);
        server.set_keep_alive_max_count(kKeepAliveMaxRequests);
        // 고정 크기 풀. 기본 풀은 요청이 몰리면 스레드를 늘린다
        server.new_task_queue = [] { return new httplib::ThreadPool(kWorkerThreadCount); };
        // 기본값은 SO_REUSEPORT라 같은 포트를 두 프로세스가 함께 열 수 있다. SO_REUSEADDR만 켜서 재시작은 허용하고 중복 실행은 막는다
        server.set_socket_options([](socket_t sock) { httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1); });
        // 16KiB 초과(413)는 httplib가 본문 없이 응답하므로, 여기서 같은 오류 봉투를 채운다
        server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
            if (res.status != 413 || !res.body.empty()) return httplib::Server::HandlerResponse::Unhandled;
            res.set_content(kPayloadTooLargeBody, "application/json");
            return httplib::Server::HandlerResponse::Handled;
        });
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
        std::cerr << "[HTTPServer] ❌ 바인딩/리슨 실패: 주소 " << address << ", 포트 " << port;
        if (cause != 0) {
            std::cerr << " (errno=" << cause << " " << std::strerror(cause) << ")" << std::endl;
        } else {
            // 진단 bind는 listen을 하지 않으므로 listen 단계 실패나 그 사이 포트 경합은 원인을 알 수 없다
            std::cerr << " (진단 bind는 성공: listen 단계 실패 또는 포트 경합, 원인 불명)" << std::endl;
        }
    }

    // 직렬화 실패(예외)도 래퍼의 try 안에서 일어나므로, 상태 코드는 본문이 만들어진 뒤에 넣는다.
    // 잘못된 UTF-8은 예외 대신 U+FFFD로 바꿔 한 기기 이름 때문에 목록 전체가 500이 되지 않게 한다.
    void writeApiResponse(httplib::Response& res, const ApiResponse& response) {
        const std::string text = response.body.dump(-1, ' ', false, json::error_handler_t::replace);
        res.status = response.httpStatus;
        res.set_content(text, "application/json");
    }

    void respondInternalError(httplib::Response& res) {
        writeApiResponse(res, ApiResponse::error(ErrorCode::Internal));
    }

    // 본문이 JSON 객체가 아니면 nullopt. 예외를 던지지 않는다.
    std::optional<json> parseJsonObject(const std::string& text) {
        json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
        return parsed;
    }

    // 문자열 필드가 없거나 문자열이 아니면 nullopt
    std::optional<std::string> readStringField(const json& object, const char* key) {
        const auto it = object.find(key);
        if (it == object.end() || !it->is_string()) return std::nullopt;
        return it->get<std::string>();
    }
}

namespace {
    const SystemClock& systemClock() {
        static const SystemClock clock;
        return clock;
    }

    std::shared_ptr<const Authenticator> requireAuthenticator(std::shared_ptr<const Authenticator> authenticator) {
        if (!authenticator) throw std::invalid_argument("HTTPServer: authenticator가 null");
        return authenticator;
    }
}

struct HTTPServer::RunState {
    httplib::Server server;
    std::atomic<bool> hasFinished{false};
};

HTTPServer::HTTPServer(DeviceManager& deviceManager, std::string apiToken, int port, std::string bindAddress)
    : HTTPServer(deviceManager, std::make_shared<BearerTokenAuthenticator>(std::move(apiToken)), port,
                 std::move(bindAddress), systemClock()) {
}

HTTPServer::HTTPServer(DeviceManager& deviceManager, std::shared_ptr<const Authenticator> authenticator, int port,
                       std::string bindAddress, const Clock& clock)
    : mDeviceManager(deviceManager), mAuthenticator(requireAuthenticator(std::move(authenticator))), mAuthFailureMonitor(clock),
      mPort(port), mBindAddress(std::move(bindAddress)) {
}

std::string HTTPServer::clientAddressOf(const httplib::Request& request) {
    return request.remote_addr;
}
HTTPServer::~HTTPServer() {
    stop();
}

std::shared_ptr<HTTPServer::RunState> HTTPServer::currentRunState() const {
    std::lock_guard<std::mutex> lock(mRunStateMutex);
    return mRunState;
}

bool HTTPServer::start(){
    if (mPort < 1 || mPort > 65535) {
        std::cerr << "[HTTPServer] ❌ 포트 범위 오류 (" << mPort << ", 1~65535)" << std::endl;
        return false;
    }
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    if (mServerThread.joinable()) {
        if (!mRunState->hasFinished) return false; // 이미 실행 중
        mServerThread.join();                      // 스스로 끝난 이전 실행을 정리하고 다시 시작
    }
    auto state = buildRunState();
    // 바인딩은 호출한 스레드에서 끝내 실패를 바로 돌려준다(httplib는 bind 때 listen까지 마친다)
    if (!state->server.bind_to_port(mBindAddress, mPort)) {
        logBindFailure(mBindAddress, mPort);
        return false;
    }
    // waitUntilReady가 true가 되기 전에 출력해 로그가 항상 준비 완료보다 앞선다
    std::cout << "[HTTPServer] HTTP 서버 시작 (주소: " << mBindAddress << ", 포트: " << mPort
              << ", 평문 HTTP)" << std::endl;
    try {
        mServerThread = std::thread(&HTTPServer::run, this, state);
    } catch (const std::system_error& e) {
        std::cerr << "[HTTPServer] ❌ 서버 스레드 생성 실패 (" << e.what() << ")" << std::endl;
        state->server.stop(); // 바인딩한 소켓을 닫는다
        return false;
    }
    {
        std::lock_guard<std::mutex> stateLock(mRunStateMutex);
        mRunState = state;
    }
    return true;
}

void HTTPServer::stop(){
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    if (!mServerThread.joinable()) return; // 시작한 적이 없거나 이미 멈춤

    // stop()은 listen 진입 전에 불려도 리슨 소켓을 닫아, 수락 스레드가 곧바로 끝난다.
    // mRunState는 생명주기 락을 쥔 쪽만 바꾸므로 여기서는 락 없이 읽어도 안전하다.
    mRunState->server.stop();
    mServerThread.join();
    std::cout << "[HTTPServer] HTTP 서버 종료" << std::endl;
}

bool HTTPServer::waitUntilReady(std::chrono::milliseconds timeout){
    const auto state = currentRunState(); // 락은 복사하는 순간만. 아래 대기는 락 밖
    if (!state) return false; // start()가 성공한 적 없음
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!state->server.is_running()) {
        if (state->hasFinished || std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

std::shared_ptr<HTTPServer::RunState> HTTPServer::buildRunState(){
    auto state = std::make_shared<RunState>();
    state->server.set_payload_max_length(16 * 1024);
    applyConnectionLimits(state->server);
    registerRoutes(state->server);
    return state;
}

void HTTPServer::registerRoutes(httplib::Server& server){
    addRoute(server, HttpMethod::Get, "/api/devices", [this](const httplib::Request&) {
        return handleGetDevices();
    });
    addRoute(server, HttpMethod::Post, "/api/control", [this](const httplib::Request& req) {
        return handleControlDevice(req.body);
    });
    addRoute(server, HttpMethod::Post, "/api/commission", [this](const httplib::Request& req) {
        return handleCommissionDevice(req.body);
    });
    addRoute(server, HttpMethod::Post, "/api/devices/delete", [this](const httplib::Request& req) {
        return handleDeleteDevice(req.body);
    });
    addRoute(server, HttpMethod::Get, R"(/api/devices/([^/]+)/state)", [this](const httplib::Request& req) {
        return handleGetDeviceState(req);
    });
    addRoute(server, HttpMethod::Get, R"(/api/devices/([^/]+)/energy)", [this](const httplib::Request& req) {
        return handleGetDeviceEnergy(req);
    });
    addRoute(server, HttpMethod::Get, R"(/api/devices/([^/]+)/energy/history)", [this](const httplib::Request& req) {
        return handleGetDeviceEnergyHistory(req);
    });

    // 없는 경로도 토큰이 틀리면 401이어야 하므로, 마지막에 전체 매칭 라우트를 둔다(앞선 라우트가 먼저 매칭된다).
    const auto notFound = [](const httplib::Request&) { return ApiResponse::error(ErrorCode::NotFound); };
    for (HttpMethod method : {HttpMethod::Get, HttpMethod::Post, HttpMethod::Put,
                              HttpMethod::Delete, HttpMethod::Patch, HttpMethod::Options}) {
        addRoute(server, method, ".*", notFound);
    }
}

void HTTPServer::run(std::shared_ptr<RunState> state){
    state->server.listen_after_bind(); // stop()이 부르면 반환한다
    state->hasFinished = true;
}

void HTTPServer::addRoute(httplib::Server& svr, HttpMethod method, const std::string& pattern, RouteHandler handler) {
    auto guarded = [this, handler = std::move(handler)](const httplib::Request& req, httplib::Response& res) {
        if (!mAuthenticator->isAuthorized(req.get_header_value("Authorization"))) {
            // 제시된 토큰은 일부라도 로그에 남기지 않는다. 메서드·경로는 외부 입력이라 정제한다.
            std::cerr << "[HTTPServer] 인증 실패: " << clientAddressOf(req) << " "
                      << RequestValidation::sanitizeForLog(req.method) << " "
                      << RequestValidation::sanitizeForLog(req.path) << std::endl;
            if (mAuthFailureMonitor.recordFailure()) {
                std::cerr << "[HTTPServer] ⚠ 인증 실패가 1분에 " << AuthFailureMonitor::kThreshold
                          << "회를 넘었습니다. 토큰 추측 시도일 수 있습니다." << std::endl;
            }
            writeApiResponse(res, ApiResponse::error(ErrorCode::Unauthorized));
            res.set_header("WWW-Authenticate", "Bearer");
            return;
        }
        // 핸들러가 처리하지 못한 예외는 내부 정보 없이 500으로 바꾼다(로그에도 메시지만 남긴다)
        try {
            writeApiResponse(res, handler(req));
        } catch (const std::exception& e) {
            std::cerr << "[HTTPServer] 처리 중 예외: " << RequestValidation::sanitizeForLog(req.method) << " "
                      << RequestValidation::sanitizeForLog(req.path) << " (" << RequestValidation::sanitizeForLog(e.what()) << ")" << std::endl;
            respondInternalError(res);
        } catch (...) {
            std::cerr << "[HTTPServer] 처리 중 알 수 없는 예외: " << RequestValidation::sanitizeForLog(req.method) << " "
                      << RequestValidation::sanitizeForLog(req.path) << std::endl;
            respondInternalError(res);
        }
    };
    switch (method) {
        case HttpMethod::Get:     svr.Get(pattern, guarded); break;
        case HttpMethod::Post:    svr.Post(pattern, guarded); break;
        case HttpMethod::Put:     svr.Put(pattern, guarded); break;
        case HttpMethod::Delete:  svr.Delete(pattern, guarded); break;
        case HttpMethod::Patch:   svr.Patch(pattern, guarded); break;
        case HttpMethod::Options: svr.Options(pattern, guarded); break;
    }
}

ApiResponse HTTPServer::handleGetDevices(){
    std::cout << "[HTTPServer] 기기 목록 조회(GET) 요청 수신" << std::endl;
    json devices = json::array();
    const std::vector<Device> deviceList = mDeviceManager.getAllDevices();
    for (const auto& device : deviceList) {
        json item;
        item["deviceId"] = device.id;
        item["name"] = device.name;
        item["protocolType"] = mDeviceManager.getProtocolString(device.protocol_type);
        item["state"] = device.state;
        devices.push_back(std::move(item));
    }
    json response;
    response["totalCount"] = deviceList.size();
    response["devices"] = std::move(devices);
    return ApiResponse::ok(std::move(response));
}

ApiResponse HTTPServer::handleControlDevice(const std::string& requestBody){
    const auto body = parseJsonObject(requestBody);
    const auto deviceId = body ? readStringField(*body, "deviceId") : std::nullopt;
    const auto command = body ? readStringField(*body, "command") : std::nullopt;
    if (!deviceId || !command) return ApiResponse::error(ErrorCode::InvalidRequest);
    if (!isAllowedControlCommand(*command)) {
        std::cerr << "[HTTPServer] 허용되지 않은 명령 거부: " << RequestValidation::sanitizeForLog(*deviceId) << std::endl;
        return ApiResponse::error(ErrorCode::InvalidRequest);
    }
    std::cout << "[HTTPServer] 제어(POST) 요청: 기기 " << RequestValidation::sanitizeForLog(*deviceId)
              << ", 명령 " << *command << std::endl;
    const CommandResult result = mDeviceManager.executeCommand(*deviceId, *command);
    return controlResponseFor(result, *deviceId, *command);
}

// 성공·실패 본문은 v1.0 그대로다(비동기 전환은 13번). 입력 검증 오류만 새 오류 봉투를 쓴다
ApiResponse HTTPServer::handleCommissionDevice(const std::string& requestBody){
    // Body에 Wi-Fi 비밀번호와 페어링 코드가 있으므로 내용을 출력하지 않는다.
    std::cout << "[HTTPServer] 기기 등록(POST) 요청 수신" << std::endl;
    const auto body = parseJsonObject(requestBody);
    if (!body) return ApiResponse::error(ErrorCode::InvalidRequest);

    const auto input = RequestValidation::parseCommission(*body);
    if (!input.ok()) {
        std::cerr << "[HTTPServer] 등록 요청 검증 실패: " << input.error.message << std::endl;
        return ApiResponse::error(input.error.code);
    }
    const CommissionInput& request = *input.value;
    const bool isSuccess = mDeviceManager.startCommissioning(request.protocolType, request.deviceName, request.payload,
                                                             request.ssid, request.password, request.driverTypes);
    if (isSuccess) return ApiResponse::ok(json{{"result", "success"}, {"message", "기기 등록 성공!"}});
    return ApiResponse::ok(json{{"result", "error"}, {"message", "기기 등록 실패!"}}, 400);
}

// 성공·실패 본문은 v1.0 그대로다(숨김 삭제는 12번). JSON 오류만 새 오류 봉투를 쓴다
ApiResponse HTTPServer::handleDeleteDevice(const std::string& requestBody) {
    const auto body = parseJsonObject(requestBody);
    const auto deviceId = body ? readStringField(*body, "deviceId") : std::nullopt;
    if (!deviceId) return ApiResponse::error(ErrorCode::InvalidRequest);
    std::cout << "[HTTPServer] 기기 삭제(POST) 요청: 기기 " << RequestValidation::sanitizeForLog(*deviceId) << std::endl;
    if (mDeviceManager.removeDevice(*deviceId).ok()) {
        return ApiResponse::ok(json{{"result", "success"}, {"message", "기기 삭제 성공!"}});
    }
    return ApiResponse::ok(json{{"result", "error"}, {"message", "기기 삭제 실패 또는 기기 없음"}}, 400);
}

ApiResponse HTTPServer::handleGetDeviceState(const httplib::Request& request) {
    const std::string& deviceId = request.matches[1];
    if (!RequestValidation::isValidDeviceId(deviceId)) return ApiResponse::error(ErrorCode::NotFound);

    // 안드로이드가 ?isManualRefresh=true 형태로 보냈을 때만 수동 새로고침
    const bool isManualRefresh = request.get_param_value("isManualRefresh") == "true";

    const auto state = mDeviceManager.refreshState(deviceId, isManualRefresh);
    if (!state.ok()) return ApiResponse::error(deviceCallErrorCode(state.error.code));
    return ApiResponse::ok(json{{"deviceId", deviceId}, {"state", *state.value}});
}

ApiResponse HTTPServer::handleGetDeviceEnergy(const httplib::Request& request) {
    const std::string& deviceId = request.matches[1];
    if (!RequestValidation::isValidDeviceId(deviceId)) return ApiResponse::error(ErrorCode::NotFound);
    std::cout << "[HTTPServer] 기기 전력량(실시간/누적) GET 요청 수신 Device ID: " << deviceId << std::endl;

    // 존재 확인을 먼저 하므로, 없는 기기는 DB에 아무것도 남기지 않는다
    const auto energy = mDeviceManager.readEnergy(deviceId);
    if (!energy.ok()) return ApiResponse::error(deviceCallErrorCode(energy.error.code));
    json response;
    response["deviceId"] = deviceId;
    response["state"] = energy.value->deviceState;
    response["activePower_mW"] = energy.value->activePower_mW;
    response["totalEnergy_mWh"] = energy.value->totalEnergy_mWh; // DB 보정이 끝난 진짜 데이터
    return ApiResponse::ok(std::move(response));
}

// from·to·period와 v2.0 키·시각 형식은 아직 적용하지 않는다(v1.0 본문). 파라미터 검사와 limit만 반영한다
ApiResponse HTTPServer::handleGetDeviceEnergyHistory(const httplib::Request& request) {
    const std::string& deviceId = request.matches[1];
    if (!RequestValidation::isValidDeviceId(deviceId)) return ApiResponse::error(ErrorCode::NotFound);
    std::cout << "[HTTPServer] 시계열 데이터 GET 요청 수신 Device ID: " << deviceId << std::endl;

    const auto query = RequestValidation::parseHistoryQuery(request.params);
    if (!query.ok()) return ApiResponse::error(query.error.code);

    const auto history = mDeviceManager.getEnergyHistory(deviceId, *query.value);
    if (!history.ok()) return ApiResponse::error(history.error.code);
    json logsArray = json::array();
    for (const auto& log : history.value->logs) {
        logsArray.push_back(json{{"timestamp", log.timestamp}, {"totalEnergy_mWh", log.total_mwh}});
    }
    json response;
    response["device_status"] = history.value->deviceState;
    response["logs"] = std::move(logsArray);
    return ApiResponse::ok(std::move(response));
}
