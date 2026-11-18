#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include "Command.hpp"
class DeviceManager;
namespace httplib { class Server; struct Request; struct Response; }

// /api/control 응답의 message 문구. 차단은 v1.0 호환을 위해 성공 문구를 쓴다.
std::string controlMessageFor(const CommandResult& result);

class HTTPServer{
public:
    HTTPServer(DeviceManager& deviceManager, std::string apiToken, int port = 8000);
    ~HTTPServer();

    bool start();
    void stop();
    // listen이 시작될 때까지 기다린다. 포트 바인드 실패 등으로 시간 안에 못 열리면 false.
    bool waitUntilReady(std::chrono::milliseconds timeout);

private:
DeviceManager& mDeviceManager;
    std::string mApiToken;
    int mPort;
    std::atomic<bool> mIsRunning;
    std::thread mServerThread;
    std::unique_ptr<httplib::Server> mServer;
    std::atomic<bool> mHasRunFinished{false};

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
