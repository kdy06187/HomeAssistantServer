#include <gtest/gtest.h>
#include "DatabaseManager.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "HTTPServer.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "PCDriver.hpp"
#include "support/RawHttpClient.hpp"
#include "httplib.h"
#include "json.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace testsupport;

namespace {

const std::string kToken = "test-token-0123456789abcdef0123456789";
const char* kUnauthorizedBody = R"({"error": "인증이 필요합니다."})";
const char* kDeviceNotFoundBody = R"({"error":{"code":"DEVICE_NOT_FOUND","message":"등록되지 않은 기기입니다."}})";
const char* kNotFoundBody = R"({"error":{"code":"NOT_FOUND","message":"없는 경로입니다."}})";
const char* kInvalidRequestBody = R"({"error":{"code":"INVALID_REQUEST","message":"요청 형식이 올바르지 않습니다."}})";

std::string makeBody(size_t size) {
    std::string body = R"({"deviceId":"X","command":"ON","pad":")";
    const std::string tail = R"("})";
    if (size > body.size() + tail.size()) body.append(size - body.size() - tail.size(), 'a');
    return body + tail;
}

class HttpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(database.init(":memory:"));
        manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
        manager.registerDriver(ProtocolType::MATTER, &matterFake);
        port = findFreePort();
        server = std::make_unique<HTTPServer>(manager, kToken, port);
        ASSERT_TRUE(server->start());
        ASSERT_TRUE(server->waitUntilReady(std::chrono::seconds(3)));
    }
    void TearDown() override { server->stop(); }

    static httplib::Headers authHeaders() { return {{"Authorization", "Bearer " + kToken}}; }

    httplib::Result post(const std::string& path, const std::string& body) {
        httplib::Client client("127.0.0.1", port);
        return client.Post(path, authHeaders(), body, "application/json");
    }

    static httplib::Result send(httplib::Client& client, const std::string& method, const std::string& path,
                                const httplib::Headers& headers, const std::string& body) {
        if (method == "GET") return client.Get(path, headers);
        if (method == "POST") return client.Post(path, headers, body, "application/json");
        if (method == "PUT") return client.Put(path, headers, body, "application/json");
        if (method == "PATCH") return client.Patch(path, headers, body, "application/json");
        return client.Delete(path, headers);
    }

    void addDevice(ProtocolType type, const std::string& id, const std::string& name,
                   const std::vector<DriverType>& driverTypes = {}) {
        IMessageSender* sender = type == ProtocolType::TCP_DIY ? static_cast<IMessageSender*>(&tcpFake)
                                                                : static_cast<IMessageSender*>(&matterFake);
        manager.addDevice(id, name, type, driverTypes, sender);
    }

    void registerPcDriver() {
        deviceDrivers.registerDriver(DriverType::PC_DRIVER, [this](IMessageSender*) {
            return std::make_shared<PCDriver>(&tcpFake);
        });
    }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DeviceManager manager{database, deviceDrivers, clock};
    FakeProtocolDriver tcpFake;
    FakeProtocolDriver matterFake;
    std::unique_ptr<HTTPServer> server;
    int port = 0;
};

TEST_F(HttpServerTest, RawSocketBadTokenWithSmallBodyGets401AndKeepAliveWorks) {
    RawHttpClient connection(port, 3);
    ASSERT_TRUE(connection.connected());
    connection.sendAll(makePostRequest("/api/control", "wrong", makeBody(100)));
    const std::string first = connection.readResponse();
    EXPECT_EQ(connection.sendErrno, 0);
    EXPECT_EQ(connection.receiveErrno, 0) << "recv errno=" << connection.receiveErrno;
    EXPECT_EQ(first.rfind("HTTP/1.1 401", 0), 0u) << first;
    EXPECT_NE(first.find(kUnauthorizedBody), std::string::npos) << first;

    connection.sendAll(makeGetRequest("/api/devices", kToken));
    const std::string second = connection.readResponse();
    EXPECT_EQ(second.rfind("HTTP/1.1 200", 0), 0u) << "second request: " << second;
}

TEST_F(HttpServerTest, RawSocketBadTokenWith8KiBBodyGets401AndKeepAliveWorks) {
    RawHttpClient connection(port, 3);
    ASSERT_TRUE(connection.connected());
    connection.sendAll(makePostRequest("/api/control", "wrong", makeBody(8 * 1024)));
    const std::string first = connection.readResponse();
    EXPECT_EQ(connection.sendErrno, 0);
    EXPECT_EQ(connection.receiveErrno, 0) << "recv errno=" << connection.receiveErrno;
    EXPECT_EQ(first.rfind("HTTP/1.1 401", 0), 0u) << first;
    EXPECT_NE(first.find(kUnauthorizedBody), std::string::npos) << first;

    connection.sendAll(makeGetRequest("/api/devices", kToken));
    const std::string second = connection.readResponse();
    EXPECT_EQ(second.rfind("HTTP/1.1 200", 0), 0u) << "second request: " << second;
}

TEST_F(HttpServerTest, ClientBadTokenWithBodyGets401ThenSameClientWorks) {
    httplib::Client client("127.0.0.1", port);
    client.set_keep_alive(true);
    for (size_t size : {size_t(100), size_t(8 * 1024)}) {
        auto response = client.Post("/api/control", httplib::Headers{{"Authorization", "Bearer wrong"}},
                                    makeBody(size), "application/json");
        ASSERT_TRUE(response) << "size " << size << " err=" << httplib::to_string(response.error());
        EXPECT_EQ(response->status, 401);
        EXPECT_EQ(response->body, kUnauthorizedBody);
        auto next = client.Get("/api/devices", authHeaders());
        ASSERT_TRUE(next) << "size " << size << " err=" << httplib::to_string(next.error());
        EXPECT_EQ(next->status, 200);
    }
}

// ---- 인증: 모든 라우트와 없는 경로 ----

struct RouteCase {
    std::string method;
    std::string path;
};

const std::vector<RouteCase> kApiRoutes = {
    {"GET", "/api/devices"},
    {"POST", "/api/control"},
    {"POST", "/api/commission"},
    {"POST", "/api/devices/delete"},
    {"GET", "/api/devices/X/state"},
    {"GET", "/api/devices/X/energy"},
    {"GET", "/api/devices/X/energy/history"},
};
const std::vector<std::string> kMethods = {"GET", "POST", "PUT", "DELETE", "PATCH"};

TEST_F(HttpServerTest, EveryRouteRejectsMissingOrWrongToken) {
    httplib::Client client("127.0.0.1", port);
    for (const auto& route : kApiRoutes) {
        for (const httplib::Headers& headers : {httplib::Headers{}, httplib::Headers{{"Authorization", "Bearer wrong"}}}) {
            auto response = send(client, route.method, route.path, headers, "{}");
            ASSERT_TRUE(response) << route.method << " " << route.path;
            EXPECT_EQ(response->status, 401) << route.method << " " << route.path;
            EXPECT_EQ(response->body, kUnauthorizedBody);
            EXPECT_EQ(response->get_header_value("WWW-Authenticate"), "Bearer");
        }
    }
}

TEST_F(HttpServerTest, UnknownPathRejectsBadTokenWith401ForEveryMethod) {
    httplib::Client client("127.0.0.1", port);
    for (const auto& method : kMethods) {
        for (const httplib::Headers& headers : {httplib::Headers{}, httplib::Headers{{"Authorization", "Bearer wrong"}}}) {
            auto response = send(client, method, "/no/such/path", headers, "{}");
            ASSERT_TRUE(response) << method;
            EXPECT_EQ(response->status, 401) << method;
            EXPECT_EQ(response->body, kUnauthorizedBody) << method;
            EXPECT_EQ(response->get_header_value("WWW-Authenticate"), "Bearer") << method;
        }
    }
}

TEST_F(HttpServerTest, UnknownPathWithValidTokenReturns404WithoutBody) {
    httplib::Client client("127.0.0.1", port);
    for (const auto& method : kMethods) {
        auto response = send(client, method, "/no/such/path", authHeaders(), "{}");
        ASSERT_TRUE(response) << method;
        EXPECT_EQ(response->status, 404) << method;
        EXPECT_TRUE(response->body.empty()) << method;
    }
}

// 원시 요청으로 상태 줄만 확인한다. 토큰 없음/틀림/정상 세 경우를 한 번에 본다.
static std::string rawStatusLine(int port, const std::string& method, const std::string& path, const std::string& authHeader) {
    RawHttpClient connection(port, 3);
    if (!connection.connected()) return "CONNECT_FAIL";
    connection.sendAll(method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n" + authHeader +
                       "Connection: close\r\n\r\n");
    const std::string response = connection.readResponse();
    return response.substr(0, response.find("\r\n"));
}

TEST_F(HttpServerTest, HeadAndOptionsRejectMissingOrWrongToken) {
    for (const std::string method : {"HEAD", "OPTIONS"}) {
        for (const std::string path : {"/api/devices", "/api/control", "/no/such/path"}) {
            EXPECT_EQ(rawStatusLine(port, method, path, ""), "HTTP/1.1 401 Unauthorized") << method << " " << path;
            EXPECT_EQ(rawStatusLine(port, method, path, "Authorization: Bearer wrong\r\n"), "HTTP/1.1 401 Unauthorized")
                << method << " " << path;
        }
    }
}

TEST_F(HttpServerTest, HeadWithValidTokenSucceedsOnGetRoute) {
    EXPECT_EQ(rawStatusLine(port, "HEAD", "/api/devices", "Authorization: Bearer " + kToken + "\r\n"), "HTTP/1.1 200 OK");
}

TEST_F(HttpServerTest, PathVariantsRejectBadTokenWith401) {
    const std::vector<std::string> paths = {
        "/api/devices/", "/api/devices?x=1", "/api/devices?isManualRefresh=true", "/API/devices", "/api/Devices",
        "//api/devices", "/api/devices//state", "/api/devices/X/state/", "/api/devices/X/energy/history/",
        "/", "/index.html", "/static/a.txt", "/../etc/passwd", "/api/devices/%2e%2e/state", "/api/control/",
        "/api/devices/delete/", "/api/devices/delete?x=1"};
    for (const auto& path : paths) {
        EXPECT_EQ(rawStatusLine(port, "GET", path, ""), "HTTP/1.1 401 Unauthorized") << "GET " << path;
        EXPECT_EQ(rawStatusLine(port, "GET", path, "Authorization: Bearer wrong\r\n"), "HTTP/1.1 401 Unauthorized")
            << "GET " << path;
        EXPECT_EQ(rawStatusLine(port, "POST", path, "Content-Length: 0\r\n"), "HTTP/1.1 401 Unauthorized") << "POST " << path;
    }
}

TEST_F(HttpServerTest, UnregisteredMethodsDoNotLeakWithoutToken) {
    // TRACE/CONNECT는 라우트가 없다. 어떤 상태가 나오든 200이어서는 안 된다(관찰값 출력).
    for (const std::string method : {"TRACE", "CONNECT", "PROPFIND"}) {
        const std::string line = rawStatusLine(port, method, "/api/devices", "");
        std::cout << "[observe] " << method << " no token -> " << line << std::endl;
        EXPECT_EQ(line.find("200"), std::string::npos) << method << ": " << line;
    }
}

TEST_F(HttpServerTest, Http10RequestWithoutTokenGets401) {
    RawHttpClient connection(port, 3);
    ASSERT_TRUE(connection.connected());
    connection.sendAll("GET /api/devices HTTP/1.0\r\n\r\n");
    EXPECT_EQ(connection.readResponse().rfind("HTTP/1.1 401", 0), 0u);
}

TEST_F(HttpServerTest, StopTwiceAndStopBeforeStartAreSafe) {
    DeviceManager other(database, deviceDrivers, clock);
    HTTPServer neverStarted(other, kToken, findFreePort());
    neverStarted.stop();
    HTTPServer started(other, kToken, findFreePort());
    ASSERT_TRUE(started.start());
    started.stop();  // listen 진입 전/후 어느 쪽이든 끝나야 한다
    started.stop();
}

TEST_F(HttpServerTest, StopImmediatelyAfterStartReturnsQuickly) {
    for (int i = 0; i < 20; ++i) {
        HTTPServer quick(manager, kToken, findFreePort());
        ASSERT_TRUE(quick.start());
        const auto begin = std::chrono::steady_clock::now();
        quick.stop();
        EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(2));
    }
}

TEST_F(HttpServerTest, StopWithIdleKeepAliveConnectionReturnsQuickly) {
    RawHttpClient idle(port, 3);
    ASSERT_TRUE(idle.connected());
    idle.sendAll(makeGetRequest("/api/devices", kToken));
    idle.readResponse();
    const auto begin = std::chrono::steady_clock::now();
    server->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(3));
}

// ---- 본문 크기 상한 ----

TEST_F(HttpServerTest, BodyUpTo16KiBIsProcessed) {
    httplib::Client client("127.0.0.1", port);
    auto response = client.Post("/api/control", authHeaders(), makeBody(16 * 1024), "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
}

TEST_F(HttpServerTest, BodyOver16KiBIsRejectedWith413) {
    RawHttpClient connection(port, 3);
    ASSERT_TRUE(connection.connected());
    connection.sendAll(makePostRequest("/api/control", kToken, makeBody(16 * 1024 + 1)));
    const std::string response = connection.readResponse();
    std::cout << "[413 observation] sendErrno=" << connection.sendErrno
              << " recvErrno=" << connection.receiveErrno << " closedByPeer=" << connection.closedByPeer
              << " response=[" << response.substr(0, 60) << "]" << std::endl;
    EXPECT_EQ(response.rfind("HTTP/1.1 413", 0), 0u) << response;
}

// ---- E1 ----

TEST_F(HttpServerTest, E1ListsDevices) {
    auto empty = httplib::Client("127.0.0.1", port).Get("/api/devices", authHeaders());
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->status, 200);
    EXPECT_EQ(json::parse(empty->body), json::parse(R"({"totalCount":0,"devices":[]})"));

    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    auto one = httplib::Client("127.0.0.1", port).Get("/api/devices", authHeaders());
    ASSERT_TRUE(one);
    EXPECT_EQ(json::parse(one->body),
              json::parse(R"({"totalCount":1,"devices":[
                  {"deviceId":"FAKE_1","name":"lamp","protocolType":"TCP_DIY","state":"UNKNOWN"}]})"));
}

// ---- E2 ----

TEST_F(HttpServerTest, E2SuccessReturnsNewStateAndSuccessMessage) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    auto response = post("/api/control", R"({"deviceId":"FAKE_1","command":"ON"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"status":"ON","message":"명령 성공!"})"));
}

TEST_F(HttpServerTest, E2UnregisteredDeviceReturns200FailureWithUnknown) {
    auto response = post("/api/control", R"({"deviceId":"NOPE","command":"ON"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"status":"UNKNOWN","message":"명령 실패!"})"));
}

TEST_F(HttpServerTest, E2UnreachableDeviceReturns200FailureWithOffline) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    tcpFake.sendCommandResult = false;
    auto response = post("/api/control", R"({"deviceId":"FAKE_1","command":"ON"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"status":"OFFLINE","message":"명령 실패!"})"));
}

TEST_F(HttpServerTest, E2MissingProtocolDriverReturns200Failure) {
    DeviceManager bareManager(database, deviceDrivers, clock);
    bareManager.addDevice("BARE_1", "bare", ProtocolType::TCP_DIY, {}, nullptr);
    const int barePort = findFreePort();
    HTTPServer bareServer(bareManager, kToken, barePort);
    ASSERT_TRUE(bareServer.start());
    ASSERT_TRUE(bareServer.waitUntilReady(std::chrono::seconds(3)));

    httplib::Client client("127.0.0.1", barePort);
    auto response = client.Post("/api/control", authHeaders(), R"({"deviceId":"BARE_1","command":"ON"})", "application/json");
    bareServer.stop();
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"status":"UNKNOWN","message":"명령 실패!"})"));
}

TEST_F(HttpServerTest, E2BlockedByDeviceDriverReturns200WithSuccessMessage) {
    registerPcDriver();
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "pc", {DriverType::PC_DRIVER});
    manager.updateDeviceState("FAKE_1", "OFF");
    auto response = post("/api/control", R"({"deviceId":"FAKE_1","command":"OFF"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"status":"OFF","message":"명령 성공!"})"));
    EXPECT_TRUE(tcpFake.sentCommands.empty());
}

// 45초 지연 스레드가 테스트보다 오래 살 수 있지만, 테스트 프로세스는 그 전에 끝난다.
TEST_F(HttpServerTest, E2DelayedCommandReturns200BeforeStateChanges) {
    registerPcDriver();
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "pc", {DriverType::PC_DRIVER});
    manager.updateDeviceState("FAKE_1", "ON");
    auto response = post("/api/control", R"({"deviceId":"FAKE_1","command":"OFF"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"status":"ON","message":"명령 성공!"})"));
    EXPECT_TRUE(tcpFake.sentCommands.empty());
}

TEST_F(HttpServerTest, E2DisallowedCommandReturns400) {
    auto response = post("/api/control", R"({"deviceId":"FAKE_1","command":"REBOOT"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    EXPECT_EQ(json::parse(response->body),
              json::parse(R"({"result":"error","message":"허용되지 않은 명령입니다."})"));
}

TEST_F(HttpServerTest, E2BadJsonOrMissingFieldReturns400) {
    for (const std::string& body : {std::string("not json"), std::string("{}")}) {
        auto response = post("/api/control", body);
        ASSERT_TRUE(response) << body;
        EXPECT_EQ(response->status, 400) << body;
        EXPECT_EQ(json::parse(response->body),
                  json::parse(R"({"result":"error","message":"잘못된 JSON 형식입니다."})")) << body;
    }
}

// ---- E3 ----

TEST_F(HttpServerTest, E3CommissionSuccess) {
    auto response = post("/api/commission",
        R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":"10.0.0.1","ssid":"s","password":"p"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"result":"success","message":"기기 등록 성공!"})"));
    ASSERT_EQ(tcpFake.commissionRequests.size(), 1u);
    EXPECT_EQ(tcpFake.commissionRequests[0].name, "lamp");
}

TEST_F(HttpServerTest, E3CommissionFailureReturns400) {
    tcpFake.commissionResult = Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "fail");
    auto response = post("/api/commission", R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":""})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"result":"error","message":"기기 등록 실패!"})"));
}

TEST_F(HttpServerTest, E3UnknownProtocolAndBadJsonReturn400) {
    auto unknown = post("/api/commission", R"({"protocolType":"ZIGBEE","deviceName":"x","payload":""})");
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->status, 400);
    EXPECT_EQ(json::parse(unknown->body),
              json::parse(R"({"result":"error","message":"알 수 없는 프로토콜 타입입니다."})"));
    auto bad = post("/api/commission", "{}");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 400);
    EXPECT_EQ(json::parse(bad->body), json::parse(R"({"result":"error","message":"잘못된 JSON 형식입니다."})"));
}

// ---- E4 ----

TEST_F(HttpServerTest, E4DeleteSuccessAndMissing) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    auto ok = post("/api/devices/delete", R"({"deviceId":"FAKE_1"})");
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
    EXPECT_EQ(json::parse(ok->body), json::parse(R"({"result":"success","message":"기기 삭제 성공!"})"));

    auto missing = post("/api/devices/delete", R"({"deviceId":"FAKE_1"})");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 400);
    EXPECT_EQ(json::parse(missing->body),
              json::parse(R"({"result":"error","message":"기기 삭제 실패 또는 기기 없음"})"));

    auto bad = post("/api/devices/delete", "nope");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 400);
}

// 오프라인 TCP 기기(연결 없음 → 해제 실패)도 삭제는 성공으로 답한다
TEST_F(HttpServerTest, E4DeleteOfflineDeviceStillSucceeds) {
    addDevice(ProtocolType::TCP_DIY, "ARD_9", "switch");
    tcpFake.unpairResult = false;

    auto res = post("/api/devices/delete", R"({"deviceId":"ARD_9"})");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(json::parse(res->body), json::parse(R"({"result":"success","message":"기기 삭제 성공!"})"));
    Device device;
    EXPECT_FALSE(manager.getDevice("ARD_9", device));
}

// 포트를 이미 다른 소켓이 쓰고 있으면 start가 실패한다(main은 이때 종료한다)
TEST(HttpServerStartTest, PortInUseIsNotReady) {
    const int holder = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(holder, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 서버는 0.0.0.0에 바인드하므로 같은 주소를 막는다
    addr.sin_port = 0;
    ASSERT_EQ(bind(holder, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(listen(holder, 1), 0);
    socklen_t len = sizeof(addr);
    getsockname(holder, reinterpret_cast<sockaddr*>(&addr), &len);
    const int busyPort = ntohs(addr.sin_port);

    DatabaseManager db;
    ASSERT_TRUE(db.init(":memory:"));
    DriverManager drivers;
    FakeClock clock;
    DeviceManager manager(db, drivers, clock);
    HTTPServer server(manager, kToken, busyPort);
    testing::internal::CaptureStderr();
    EXPECT_FALSE(server.start());  // 바인딩 실패는 start가 바로 돌려준다
    testing::internal::GetCapturedStderr();

    EXPECT_FALSE(server.waitUntilReady(std::chrono::milliseconds(50)));
    server.stop();
    close(holder);
}

// ---- E5 ----

TEST_F(HttpServerTest, E5StateOfKnownAndUnknownDevice) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    tcpFake.stateToReturn = "OFF";
    httplib::Client client("127.0.0.1", port);
    auto known = client.Get("/api/devices/FAKE_1/state?isManualRefresh=true", authHeaders());
    ASSERT_TRUE(known);
    EXPECT_EQ(known->status, 200);
    EXPECT_EQ(json::parse(known->body), json::parse(R"({"deviceId":"FAKE_1","state":"OFF"})"));

    auto unknown = client.Get("/api/devices/NOPE/state", authHeaders());
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->status, 404);
    EXPECT_EQ(json::parse(unknown->body), json::parse(kDeviceNotFoundBody));
    EXPECT_EQ(unknown->get_header_value("Content-Type"), "application/json");
}

TEST_F(HttpServerTest, E5MalformedDeviceIdReturns404NotFoundForEveryReadRoute) {
    httplib::Client client("127.0.0.1", port);
    const std::string tooLong(65, 'a');
    for (const std::string& badId : {std::string("a.b"), std::string("a%20b"), tooLong}) {
        for (const std::string suffix : {"/state", "/energy", "/energy/history"}) {
            auto response = client.Get("/api/devices/" + badId + suffix, authHeaders());
            ASSERT_TRUE(response) << badId << suffix;
            EXPECT_EQ(response->status, 404) << badId << suffix;
            EXPECT_EQ(json::parse(response->body), json::parse(kNotFoundBody)) << badId << suffix;
        }
    }
}

// ---- E6 ----

TEST_F(HttpServerTest, E6EnergySuccess) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    matterFake.stateToReturn = "ON";
    matterFake.energyResult = Result<EnergyReading>::success(EnergyReading{15230, 1048576});
    manager.updateDeviceState("PLUG_1", "ON");
    auto response = httplib::Client("127.0.0.1", port).Get("/api/devices/PLUG_1/energy", authHeaders());
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body),
              json::parse(R"({"deviceId":"PLUG_1","state":"ON","activePower_mW":15230,"totalEnergy_mWh":1048576})"));
    EXPECT_TRUE(json::parse(response->body)["totalEnergy_mWh"].is_number_integer());
}

TEST_F(HttpServerTest, E6UnregisteredReturns404AndTcpDeviceReturns400Unsupported) {
    httplib::Client client("127.0.0.1", port);
    auto missing = client.Get("/api/devices/NOPE/energy", authHeaders());
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
    EXPECT_EQ(json::parse(missing->body), json::parse(kDeviceNotFoundBody));

    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    auto tcp = client.Get("/api/devices/FAKE_1/energy", authHeaders());
    ASSERT_TRUE(tcp);
    EXPECT_EQ(tcp->status, 400);
    EXPECT_EQ(json::parse(tcp->body),
              json::parse(R"({"error":{"code":"UNSUPPORTED_OPERATION","message":"이 기기에서 지원하지 않는 요청입니다."}})"));
    EXPECT_TRUE(tcpFake.energyReadIds.empty());
}

TEST_F(HttpServerTest, E6ReadFailureReturns502ForUnreachableAndParseFailure) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    httplib::Client client("127.0.0.1", port);
    for (ErrorCode code : {ErrorCode::DeviceUnreachable, ErrorCode::Internal}) {
        matterFake.energyResult = Result<EnergyReading>::failure(code, "read failed");
        auto failed = client.Get("/api/devices/PLUG_1/energy", authHeaders());
        ASSERT_TRUE(failed);
        EXPECT_EQ(failed->status, 502);
        EXPECT_EQ(json::parse(failed->body),
                  json::parse(R"({"error":{"code":"DEVICE_UNREACHABLE","message":"기기가 응답하지 않습니다."}})"));
    }
}

// ---- E7 ----

// from·to·period와 v2.0 키·시각 형식은 12번에서 바꾼다. 지금은 v1.0 본문 + 새 오류 규약
TEST_F(HttpServerTest, E7HistoryReturnsLogsAnd404ForUnknownDevice) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    database.insertEnergyLog("PLUG_1", 100);
    database.insertEnergyLog("PLUG_1", 200);
    httplib::Client client("127.0.0.1", port);
    auto response = client.Get("/api/devices/PLUG_1/energy/history", authHeaders());
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const json body = json::parse(response->body);
    EXPECT_EQ(body["device_status"], "UNKNOWN");
    ASSERT_EQ(body["logs"].size(), 2u);
    EXPECT_EQ(body["logs"][0]["totalEnergy_mWh"], 100);
    EXPECT_EQ(body["logs"][1]["totalEnergy_mWh"], 200);

    auto missing = client.Get("/api/devices/NOPE/energy/history", authHeaders());
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
    EXPECT_EQ(json::parse(missing->body), json::parse(kDeviceNotFoundBody));
}

TEST_F(HttpServerTest, E7LimitQueryIsAppliedAndInvalidQueriesReturn400) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    for (int i = 1; i <= 5; ++i) database.insertEnergyLog("PLUG_1", i * 100);
    httplib::Client client("127.0.0.1", port);

    auto limited = client.Get("/api/devices/PLUG_1/energy/history?limit=2", authHeaders());
    ASSERT_TRUE(limited);
    EXPECT_EQ(limited->status, 200);
    EXPECT_EQ(json::parse(limited->body)["logs"].size(), 2u);

    for (const std::string query : {"limit=0", "limit=abc", "from=2026-02-30&to=2026-03-01", "period=week",
                                    "from=2026-03-02&to=2026-03-01", "limit=1&limit=2"}) {
        auto response = client.Get("/api/devices/PLUG_1/energy/history?" + query, authHeaders());
        ASSERT_TRUE(response) << query;
        EXPECT_EQ(response->status, 400) << query;
        EXPECT_EQ(json::parse(response->body), json::parse(kInvalidRequestBody)) << query;
    }
}

TEST_F(HttpServerTest, E7InvalidQueryDoesNotTouchTheDatabase) {
    auto response = httplib::Client("127.0.0.1", port).Get("/api/devices/NOPE/energy/history?limit=-1", authHeaders());
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
}

// ---- CommandResult -> message 매핑 ----

TEST(ControlMessageTest, MapsOutcomeToMessage) {
    struct Case { CommandOutcome outcome; ErrorCode code; const char* message; };
    const Case cases[] = {
        {CommandOutcome::Succeeded, ErrorCode::None, "명령 성공!"},
        {CommandOutcome::Accepted, ErrorCode::None, "명령 성공!"},
        {CommandOutcome::Failed, ErrorCode::CommandBlocked, "명령 성공!"},
        {CommandOutcome::Failed, ErrorCode::DeviceNotFound, "명령 실패!"},
        {CommandOutcome::Failed, ErrorCode::DeviceUnreachable, "명령 실패!"},
        {CommandOutcome::Failed, ErrorCode::Internal, "명령 실패!"},
    };
    for (const auto& item : cases) {
        CommandResult result;
        result.outcome = item.outcome;
        result.error.code = item.code;
        EXPECT_EQ(controlMessageFor(result), item.message);
    }
}

}  // namespace
