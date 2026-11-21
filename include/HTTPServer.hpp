#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include "Command.hpp"
#include "Result.hpp"
class DeviceManager;
namespace httplib { class Server; struct Request; struct Response; }

// /api/control 응답의 message 문구. 차단은 v1.0 호환을 위해 성공 문구를 쓴다.
std::string controlMessageFor(const CommandResult& result);

// HAC_HTTP_BIND 환경 변수 값을 바인딩 주소로 바꾼다. 없거나 빈 값이면 기본 0.0.0.0, IPv4 형식이 아니면 실패.
Result<std::string> parseHttpBindAddress(const char* environmentValue);

class HTTPServer{
public:
    HTTPServer(DeviceManager& deviceManager, std::string apiToken, int port = 8000,
               std::string bindAddress = "0.0.0.0");
    ~HTTPServer();

    // 이미 실행 중이면 false. 포트 바인딩 실패는 waitUntilReady가 false로 알려 준다.
    // stop 뒤나 바인딩 실패 뒤에 다시 부를 수 있다. 여러 스레드가 동시에 불러도 안전하다.
    bool start();
    // 실행 중이 아니면 아무것도 하지 않는다(여러 번 불러도 안전).
    void stop();
    // listen이 시작될 때까지 기다린다. 포트 바인드 실패 등으로 시간 안에 못 열리면 false.
    bool waitUntilReady(std::chrono::milliseconds timeout);

private:
DeviceManager& mDeviceManager;
    std::string mApiToken;
    int mPort;
    std::string mBindAddress;
    // start/stop 호출끼리만 직렬화한다(안에서 join까지 하므로 다른 락과 겹쳐 잡지 않는다)
    std::mutex mLifecycleMutex;
    std::thread mServerThread;
    std::unique_ptr<httplib::Server> mServer;
    std::atomic<bool> mHasRunFinished{false};

    // start마다 새 서버 객체를 만들어 라우트를 한 번만 등록한다
    void buildServer();
    void registerRoutes();
    void run();

    enum class HttpMethod { Get, Post, Put, Delete, Patch, Options };
    using RouteHandler = std::function<void(const httplib::Request&, httplib::Response&)>;
    // 모든 라우트는 이 함수로만 등록한다. 인증을 통과한 요청만 handler에 도달한다.
    void addRoute(HttpMethod method, const std::string& pattern, RouteHandler handler);

    std::string handleGetDevices();
    std::string handleControlDevice(const std::string& requestBody);
    std::string handleCommissionDevice(const std::string& requestBody);
    std::string handleDeleteDevice(const std::string& requestBody);
    std::string handleGetDeviceEnergy(const std::string& deviceId);
    std::string handleGetDeviceEnergyHistory(const std::string& deviceId);
};
