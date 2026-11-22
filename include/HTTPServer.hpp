#pragma once
#include <string>
#include <thread>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include "ApiResponse.hpp"
#include "AuthFailureMonitor.hpp"
#include "Authenticator.hpp"
#include "Clock.hpp"
#include "Command.hpp"
#include "Result.hpp"
class DeviceManager;
namespace httplib { class Server; struct Request; struct Response; }

class HTTPServer{
public:
    HTTPServer(DeviceManager& deviceManager, std::string apiToken, int port = 8000,
               std::string bindAddress = "0.0.0.0");
    // 인증 방식과 시계를 주입한다(테스트용). clock은 서버보다 오래 살아야 한다.
    // authenticator가 null이면 std::invalid_argument(인증 없는 서버를 만들지 않는다)
    HTTPServer(DeviceManager& deviceManager, std::shared_ptr<const Authenticator> authenticator, int port,
               std::string bindAddress, const Clock& clock);
    ~HTTPServer();

    // 락: start는 생명주기 락을 쥔 채 실행 상태 락을 잡는다(생명주기 → 실행 상태).
    // 포트 바인딩까지 이 호출 안에서 끝내고, 성공하면 수락 스레드를 띄운다(TcpServer::start와 같은 규약:
    // true는 이번 호출이 시작했다는 뜻). 이미 실행 중이거나 포트가 1~65535 밖이거나 바인딩에 실패하면 false.
    // stop 뒤나 바인딩 실패 뒤에 다시 부를 수 있다. 여러 스레드가 동시에 불러도 안전하다.
    bool start();
    // 실행 중이 아니면 아무것도 하지 않는다(여러 번 불러도 안전).
    void stop();
    // 수락 스레드가 listen에 들어갈 때까지 기다린다. start가 성공하지 않았거나 시간 안에 못 열리면 false.
    bool waitUntilReady(std::chrono::milliseconds timeout);

private:
DeviceManager& mDeviceManager;
    std::shared_ptr<const Authenticator> mAuthenticator;
    AuthFailureMonitor mAuthFailureMonitor;
    int mPort;
    std::string mBindAddress;
    // 한 번의 start가 만드는 서버 객체와 종료 표시. 스레드·waitUntilReady가 shared_ptr로 함께 쥔다
    struct RunState;
    // start/stop 호출끼리 직렬화한다(안에서 join까지 한다). start는 이 락을 쥔 채 mRunStateMutex를 잡는다
    // (생명주기 락 → 실행 상태 락). 반대 순서로 잡는 곳은 없다
    std::mutex mLifecycleMutex;
    std::thread mServerThread;
    // mRunState 포인터 자체만 보호한다(복사·교체 순간에만 잡고 대기·join은 락 밖)
    mutable std::mutex mRunStateMutex;
    std::shared_ptr<RunState> mRunState;
    std::shared_ptr<RunState> currentRunState() const;

    // start마다 새 서버 객체를 만들어 라우트를 한 번만 등록한다
    std::shared_ptr<RunState> buildRunState();
    void registerRoutes(httplib::Server& server);
    void run(std::shared_ptr<RunState> state);
    // 요청자 주소 판정은 이 함수 한 곳에서만 한다(지금은 소켓 주소 그대로)
    static std::string clientAddressOf(const httplib::Request& request);

    enum class HttpMethod { Get, Post, Put, Delete, Patch, Options };
    using RouteHandler = std::function<ApiResponse(const httplib::Request&)>;
    // 모든 라우트는 이 함수로만 등록한다. 인증을 통과한 요청만 handler에 도달하고,
    // 응답 직렬화와 예외→500 변환은 여기서 한다.
    void addRoute(httplib::Server& server, HttpMethod method, const std::string& pattern, RouteHandler handler);

    ApiResponse handleGetDevices();
    ApiResponse handleControlDevice(const std::string& requestBody);
    ApiResponse handleCommissionDevice(const std::string& requestBody);
    ApiResponse handleDeleteDevice(const std::string& requestBody);
    ApiResponse handleGetDeviceState(const httplib::Request& request);
    ApiResponse handleGetDeviceEnergy(const httplib::Request& request);
    ApiResponse handleGetDeviceEnergyHistory(const httplib::Request& request);
};
