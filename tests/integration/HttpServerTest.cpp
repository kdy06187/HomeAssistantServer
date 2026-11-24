#include <gtest/gtest.h>
#include <algorithm>
#include <map>
#include "DatabaseManager.hpp"
#include "UtcTime.hpp"
#include "support/HistoryTestHelper.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "CommissioningService.hpp"
#include "HTTPServer.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "ChipToolOutputParser.hpp"
#include "PCDriver.hpp"
#include "support/RawHttpClient.hpp"
#include "support/LogCapture.hpp"
#include "httplib.h"
#include "json.hpp"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <atomic>
#include <thread>
#include "RequestValidation.hpp"
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
const char* kUnauthorizedBody = R"({"error":{"code":"UNAUTHORIZED","message":"인증에 실패했습니다."}})";
const char* kInternalErrorBody = R"({"error":{"code":"INTERNAL_ERROR","message":"서버 내부 오류입니다."}})";
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
        commissioning.start();
        port = findFreePort();
        server = std::make_unique<HTTPServer>(manager, commissioning, kToken, port);
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
        manager.addDevice(id, name, type, driverTypes);
    }

    void registerPcDriver() {
        deviceDrivers.registerDriver(DriverType::PC_DRIVER, [this]() {
            return std::make_shared<PCDriver>(&tcpFake);
        });
    }

    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver tcpFake;
    FakeProtocolDriver matterFake;
    // 가짜 드라이버보다 뒤에 선언해 먼저 소멸(작업 스레드 join)한다
    CommissioningService commissioning{manager, {{ProtocolType::TCP_DIY, &tcpFake}, {ProtocolType::MATTER, &matterFake}}, clock};
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
        EXPECT_EQ(json::parse(response->body), json::parse(kUnauthorizedBody));
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
    {"GET", "/api/commission/c-x-1"},
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
            EXPECT_EQ(json::parse(response->body), json::parse(kUnauthorizedBody));
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
            EXPECT_EQ(json::parse(response->body), json::parse(kUnauthorizedBody)) << method;
            EXPECT_EQ(response->get_header_value("WWW-Authenticate"), "Bearer") << method;
        }
    }
}

TEST_F(HttpServerTest, UnknownPathWithValidTokenReturns404WithErrorEnvelope) {
    httplib::Client client("127.0.0.1", port);
    for (const auto& method : kMethods) {
        auto response = send(client, method, "/no/such/path", authHeaders(), "{}");
        ASSERT_TRUE(response) << method;
        EXPECT_EQ(response->status, 404) << method;
        EXPECT_EQ(json::parse(response->body), json::parse(kNotFoundBody)) << method;
        EXPECT_EQ(response->get_header_value("Content-Type"), "application/json") << method;
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
    DelayedCommandScheduler otherScheduler{clock};
    DeviceManager other(database, deviceDrivers, otherScheduler, clock);
    CommissioningService otherCommissioning{other, {}, clock};
    HTTPServer neverStarted(other, otherCommissioning, kToken, findFreePort());
    neverStarted.stop();
    HTTPServer started(other, otherCommissioning, kToken, findFreePort());
    ASSERT_TRUE(started.start());
    started.stop();  // listen 진입 전/후 어느 쪽이든 끝나야 한다
    started.stop();
}

TEST_F(HttpServerTest, StopImmediatelyAfterStartReturnsQuickly) {
    for (int i = 0; i < 20; ++i) {
        HTTPServer quick(manager, commissioning, kToken, findFreePort());
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
    EXPECT_EQ(response->status, 404);  // 본문은 끝까지 읽혀 처리됐고, 등록되지 않은 기기 X라서 404
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
    EXPECT_NE(response.find("Content-Type: application/json"), std::string::npos) << response;
    EXPECT_NE(response.find(R"({"error":{"code":"PAYLOAD_TOO_LARGE","message":"요청 본문이 너무 큽니다."}})"), std::string::npos)
        << response;
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
                  {"deviceId":"FAKE_1","name":"lamp","protocolType":"TCP_DIY","state":"UNKNOWN","ip":null}]})"));
}

// TCP 기기는 마지막 접속 IP(문자열), Matter는 null. 재접속으로 IP가 바뀌면 목록에 반영된다
TEST_F(HttpServerTest, E1ShowsTcpIpAndNullForMatter) {
    manager.addDevice("TCP_A1B2C3D4E5F6", "lamp", ProtocolType::TCP_DIY, {}, "192.168.0.50");
    addDevice(ProtocolType::MATTER, "SN12345", "plug");
    manager.onTcpConnected("SN12345", "192.168.0.99");  // Matter와 같은 ID가 접속해도 ip를 붙이지 않는다
    auto listIps = [&] {
        auto response = httplib::Client("127.0.0.1", port).Get("/api/devices", authHeaders());
        std::map<std::string, json> ips;
        if (!response) return ips;
        const json body = json::parse(response->body);  // 임시 객체의 하위 참조를 돌면 수명이 끝난 값을 읽는다
        for (const auto& device : body["devices"]) ips[device["deviceId"].get<std::string>()] = device["ip"];
        return ips;
    };
    auto ips = listIps();
    EXPECT_EQ(ips["TCP_A1B2C3D4E5F6"], "192.168.0.50");
    EXPECT_TRUE(ips["SN12345"].is_null());

    manager.onTcpConnected("TCP_A1B2C3D4E5F6", "192.168.0.51");
    EXPECT_EQ(listIps()["TCP_A1B2C3D4E5F6"], "192.168.0.51");
}

// 2차 ID 모양(TCP_<MAC12>)도 경로 검증을 통과하고 E5 200, E6·E7 400 UNSUPPORTED_OPERATION이다
TEST_F(HttpServerTest, RealTcpMacIdPassesPathValidationForE5E6E7) {
    const std::string id = "TCP_A1B2C3D4E5F6";
    manager.addDevice(id, "lamp", ProtocolType::TCP_DIY, {});
    tcpFake.stateToReturn = "ON";
    httplib::Client client("127.0.0.1", port);
    auto state = client.Get("/api/devices/" + id + "/state", authHeaders());
    ASSERT_TRUE(state);
    EXPECT_EQ(state->status, 200);
    EXPECT_EQ(json::parse(state->body)["deviceId"], id);
    const json unsupported = json::parse(R"({"error":{"code":"UNSUPPORTED_OPERATION","message":"이 기기에서 지원하지 않는 요청입니다."}})");
    for (const std::string suffix : {"/energy", "/energy/history"}) {
        auto response = client.Get("/api/devices/" + id + suffix, authHeaders());
        ASSERT_TRUE(response) << suffix;
        EXPECT_EQ(response->status, 400) << suffix;
        EXPECT_EQ(json::parse(response->body), unsupported) << suffix;
    }
}

// 이름이 "error"여도 오류 응답으로 오인되지 않고 200 목록이다
TEST_F(HttpServerTest, E1DeviceNamedErrorIsStillA200List) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "error");
    auto response = httplib::Client("127.0.0.1", port).Get("/api/devices", authHeaders());
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body)["devices"][0]["name"], "error");
}

// 잘못된 UTF-8 이름은 U+FFFD로 바뀌어 직렬화 예외(500) 없이 목록이 나간다
TEST_F(HttpServerTest, E1InvalidUtf8NameDoesNotCauseInternalError) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", std::string("bad\xC3\x28name"));
    addDevice(ProtocolType::TCP_DIY, "FAKE_2", "ok");
    auto response = httplib::Client("127.0.0.1", port).Get("/api/devices", authHeaders());
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_NE(response->body.find("\xEF\xBF\xBD"), std::string::npos);
    EXPECT_NE(response->body.find("\"ok\""), std::string::npos);
}

// ---- E2 ----

TEST_F(HttpServerTest, E2SuccessReturns200WithDeviceIdAndNewState) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    auto response = post("/api/control", R"({"deviceId":"FAKE_1","command":"ON"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"deviceId":"FAKE_1","state":"ON"})"));
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
}

TEST_F(HttpServerTest, E2UnregisteredDeviceReturns404) {
    auto response = post("/api/control", R"({"deviceId":"NOPE","command":"ON"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_EQ(json::parse(response->body), json::parse(kDeviceNotFoundBody));
}

TEST_F(HttpServerTest, E2UnreachableDeviceReturns502) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    tcpFake.sendCommandResult = false;
    auto response = post("/api/control", R"({"deviceId":"FAKE_1","command":"ON"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 502);
    EXPECT_EQ(json::parse(response->body),
              json::parse(R"({"error":{"code":"DEVICE_UNREACHABLE","message":"기기가 응답하지 않습니다."}})"));
}

TEST_F(HttpServerTest, E2MissingProtocolDriverReturns500) {
    DelayedCommandScheduler bareManagerScheduler{clock};
    DeviceManager bareManager(database, deviceDrivers, bareManagerScheduler, clock);
    CommissioningService bareManagerCommissioning{bareManager, {}, clock};
    bareManager.addDevice("BARE_1", "bare", ProtocolType::TCP_DIY, {});
    const int barePort = findFreePort();
    HTTPServer bareServer(bareManager, bareManagerCommissioning, kToken, barePort);
    ASSERT_TRUE(bareServer.start());
    ASSERT_TRUE(bareServer.waitUntilReady(std::chrono::seconds(3)));

    httplib::Client client("127.0.0.1", barePort);
    auto response = client.Post("/api/control", authHeaders(), R"({"deviceId":"BARE_1","command":"ON"})", "application/json");
    bareServer.stop();
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_EQ(json::parse(response->body), json::parse(kInternalErrorBody));
}

TEST_F(HttpServerTest, E2BlockedByDeviceDriverReturns409) {
    registerPcDriver();
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "pc", {DriverType::PC_DRIVER});
    manager.updateDeviceState("FAKE_1", "OFF");
    auto response = post("/api/control", R"({"deviceId":"FAKE_1","command":"OFF"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 409);
    EXPECT_EQ(json::parse(response->body),
              json::parse(R"({"error":{"code":"COMMAND_BLOCKED","message":"기기 드라이버가 명령을 막았습니다."}})"));
    EXPECT_TRUE(tcpFake.sentCommands.empty());
}

TEST(HttpServerDelayedCommandTest, E2DelayedCommandReturns202WithPendingCommand) {
    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    CommissioningService commissioning{manager, {}, clock};
    FakeProtocolDriver tcpFake;
    ASSERT_TRUE(database.init(":memory:"));
    manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [&tcpFake]() {
        return std::make_shared<PCDriver>(&tcpFake);
    });
    manager.addDevice("FAKE_1", "pc", ProtocolType::TCP_DIY, {DriverType::PC_DRIVER});
    manager.updateDeviceState("FAKE_1", "ON");
    const int serverPort = findFreePort();
    HTTPServer server(manager, commissioning, kToken, serverPort);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitUntilReady(std::chrono::seconds(3)));

    httplib::Client client("127.0.0.1", serverPort);
    auto response = client.Post("/api/control", httplib::Headers{{"Authorization", "Bearer " + kToken}},
                                R"({"deviceId":"FAKE_1","command":"OFF"})", "application/json");
    server.stop();
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 202);
    EXPECT_EQ(json::parse(response->body),
              json::parse(R"({"deviceId":"FAKE_1","state":"ON","pendingCommand":"OFF","delaySeconds":45})"));
    EXPECT_TRUE(tcpFake.sentCommands.empty());
}

TEST(HttpServerDelayedCommandTest, E2SameCommandWhilePendingReturns202WithRemainingSeconds) {
    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    CommissioningService commissioning{manager, {}, clock};
    FakeProtocolDriver tcpFake;
    ASSERT_TRUE(database.init(":memory:"));
    manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [&tcpFake]() {
        return std::make_shared<PCDriver>(&tcpFake);
    });
    manager.addDevice("FAKE_1", "pc", ProtocolType::TCP_DIY, {DriverType::PC_DRIVER});
    manager.updateDeviceState("FAKE_1", "ON");
    const int serverPort = findFreePort();
    HTTPServer server(manager, commissioning, kToken, serverPort);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitUntilReady(std::chrono::seconds(3)));

    httplib::Client client("127.0.0.1", serverPort);
    const httplib::Headers headers{{"Authorization", "Bearer " + kToken}};
    const std::string body = R"({"deviceId":"FAKE_1","command":"OFF"})";
    auto first = client.Post("/api/control", headers, body, "application/json");
    clock.advance(std::chrono::seconds(10));
    auto second = client.Post("/api/control", headers, body, "application/json");
    server.stop();

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 202);
    EXPECT_EQ(json::parse(second->body),
              json::parse(R"({"deviceId":"FAKE_1","state":"ON","pendingCommand":"OFF","delaySeconds":35})"));
    EXPECT_EQ(tcpFake.pcAgentMessages.size(), 1u);
}

// 종료 중 취소된 기기 호출이 200(성공 상태 코드)으로 나가지 않고 502로 나간다
TEST(HttpServerDelayedCommandTest, E2DuringShutdownReturns502NotCancelled200) {
    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    CommissioningService commissioning{manager, {}, clock};
    FakeProtocolDriver tcpFake;
    ASSERT_TRUE(database.init(":memory:"));
    manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [&tcpFake]() {
        return std::make_shared<PCDriver>(&tcpFake);
    });
    manager.addDevice("FAKE_1", "pc", ProtocolType::TCP_DIY, {DriverType::PC_DRIVER});
    manager.updateDeviceState("FAKE_1", "ON");
    manager.beginShutdown();
    scheduler.stopAndCancelAll();
    const int serverPort = findFreePort();
    HTTPServer server(manager, commissioning, kToken, serverPort);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitUntilReady(std::chrono::seconds(3)));

    httplib::Client client("127.0.0.1", serverPort);
    auto response = client.Post("/api/control", httplib::Headers{{"Authorization", "Bearer " + kToken}},
                                R"({"deviceId":"FAKE_1","command":"OFF"})", "application/json");
    server.stop();

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 502);
    EXPECT_EQ(json::parse(response->body),
              json::parse(R"({"error":{"code":"DEVICE_UNREACHABLE","message":"기기가 응답하지 않습니다."}})"));
    EXPECT_TRUE(tcpFake.pcAgentMessages.empty());
}

TEST_F(HttpServerTest, E2InvalidRequestsReturn400Envelope) {
    const std::vector<std::string> bodies = {
        R"({"deviceId":"FAKE_1","command":"REBOOT"})",  // 허용되지 않은 명령
        "not json", "{}", "[]", "null",
        R"({"deviceId":"FAKE_1"})", R"({"command":"ON"})",
        R"({"deviceId":1,"command":"ON"})", R"({"deviceId":"FAKE_1","command":true})"};
    for (const std::string& body : bodies) {
        auto response = post("/api/control", body);
        ASSERT_TRUE(response) << body;
        EXPECT_EQ(response->status, 400) << body;
        EXPECT_EQ(json::parse(response->body), json::parse(kInvalidRequestBody)) << body;
    }
}

// ---- E3 ----

// 이 기기의 사설 IPv4 하나(없으면 빈 문자열). 빈 payload가 "접속한 서버 주소"로 바뀌는지 볼 때 쓴다
std::string firstLanAddressOfThisHost() {
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return "";
    std::string found;
    for (ifaddrs* item = list; item && found.empty(); item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET) continue;
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr, text, sizeof(text));
        if (RequestValidation::isLanIpv4Address(text)) found = text;
    }
    freeifaddrs(list);
    return found;
}

// E8이 끝난 상태(SUCCEEDED·FAILED)가 될 때까지(최대 5초) 조회한다
json waitJobFinished(int port, const std::string& jobId) {
    httplib::Client client("127.0.0.1", port);
    json last;
    for (int i = 0; i < 500; ++i) {
        auto response = client.Get("/api/commission/" + jobId, httplib::Headers{{"Authorization", "Bearer " + kToken}});
        if (!response || response->status != 200) return json();
        last = json::parse(response->body);
        if (last["state"] == "SUCCEEDED" || last["state"] == "FAILED") return last;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return last;
}

// 접수는 바로 202 + jobId, 결과는 E8
TEST_F(HttpServerTest, E3AcceptsWith202AndE8ReportsSuccess) {
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"TCP_A1B2C3D4E5F6"});
    auto response = post("/api/commission",
        R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":"10.0.0.1","ssid":"s","password":"p"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 202);
    const json accepted = json::parse(response->body);
    EXPECT_EQ(accepted["state"], "PENDING");
    const std::string jobId = accepted["jobId"];
    EXPECT_TRUE(RequestValidation::isValidJobId(jobId)) << jobId;

    const json finished = waitJobFinished(port, jobId);
    EXPECT_EQ(finished, json::parse(R"({"jobId":")" + jobId +
                                    R"(","state":"SUCCEEDED","deviceId":"TCP_A1B2C3D4E5F6","restored":false,"error":null})"));
    ASSERT_EQ(tcpFake.commissionRequests.size(), 1u);
    EXPECT_EQ(tcpFake.commissionRequests[0].name, "lamp");
    EXPECT_EQ(tcpFake.commissionRequests[0].ssid, "s");
    EXPECT_EQ(tcpFake.commissionRequests[0].payload, "10.0.0.1");  // 명시한 IP는 그대로
}

// MATTER 설정 코드 형식(11자리·21자리·MT:)과 driverTypes가 등록까지 전달된다
TEST_F(HttpServerTest, E3MatterPayloadFormatsSucceedAndDriverTypesAreForwarded) {
    registerPcDriver();
    int index = 0;
    for (const std::string& payload : {std::string(11, '1'), std::string(21, '2'), std::string("MT:Y.K9042C00KA0648G00")}) {
        matterFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"PLUG_" + std::to_string(++index)});
        auto response = post("/api/commission", R"({"protocolType":"MATTER","deviceName":"plug","payload":")" + payload +
                                                    R"(","driverTypes":[1]})");
        ASSERT_TRUE(response) << payload;
        ASSERT_EQ(response->status, 202) << payload;
        EXPECT_EQ(waitJobFinished(port, json::parse(response->body)["jobId"])["state"], "SUCCEEDED") << payload;
        EXPECT_EQ(matterFake.commissionRequests.back().payload, payload);
        Device device;
        ASSERT_TRUE(manager.getDevice("PLUG_" + std::to_string(index), device));
        EXPECT_EQ(device.driver_types, std::vector<DriverType>{DriverType::PC_DRIVER});
    }
}

TEST_F(HttpServerTest, E8ReportsFailureWithFixedMessageAndRestored) {
    tcpFake.commissionResult = Result<CommissionOutcome>::failure(ErrorCode::CommissionTimeout, "timeout");
    auto failed = post("/api/commission", R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":"10.0.0.1"})");
    ASSERT_TRUE(failed);
    const json failedJob = waitJobFinished(port, json::parse(failed->body)["jobId"]);
    EXPECT_EQ(failedJob["state"], "FAILED");
    EXPECT_EQ(failedJob["deviceId"], nullptr);
    EXPECT_EQ(failedJob["error"], json::parse(R"({"code":"COMMISSION_TIMEOUT","message":"기기 등록 시간이 초과되었습니다."})"));

    addDevice(ProtocolType::TCP_DIY, "TCP_000000000001", "lamp");
    ASSERT_TRUE(manager.removeDevice("TCP_000000000001").ok());
    tcpFake.commissionResult = Result<CommissionOutcome>::success(CommissionOutcome{"TCP_000000000001"});
    auto restored = post("/api/commission", R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":"10.0.0.1"})");
    ASSERT_TRUE(restored);
    const json restoredJob = waitJobFinished(port, json::parse(restored->body)["jobId"]);
    EXPECT_EQ(restoredJob["state"], "SUCCEEDED");
    EXPECT_EQ(restoredJob["restored"], true);

    auto already = post("/api/commission", R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":"10.0.0.1"})");
    ASSERT_TRUE(already);
    EXPECT_EQ(waitJobFinished(port, json::parse(already->body)["jobId"])["error"]["code"], "ALREADY_REGISTERED");
}

TEST_F(HttpServerTest, E8UnknownOrMalformedJobIdIs404JobNotFound) {
    httplib::Client client("127.0.0.1", port);
    const json expected = json::parse(R"({"error":{"code":"JOB_NOT_FOUND","message":"없는 등록 작업입니다."}})");
    for (const std::string& jobId : {std::string("c-unknown-1"), std::string("UPPER"), std::string(33, 'a'), std::string("a_b")}) {
        auto response = client.Get("/api/commission/" + jobId, authHeaders());
        ASSERT_TRUE(response) << jobId;
        EXPECT_EQ(response->status, 404) << jobId;
        EXPECT_EQ(json::parse(response->body), expected) << jobId;
    }
}

TEST_F(HttpServerTest, E3WhileAnotherJobRunsIs409Busy) {
    std::atomic<bool> release{false};
    tcpFake.onCommission = [&] {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    auto first = post("/api/commission", R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":"10.0.0.1"})");
    ASSERT_TRUE(first);
    EXPECT_EQ(first->status, 202);
    auto second = post("/api/commission", R"({"protocolType":"MATTER","deviceName":"plug","payload":"11111111111"})");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 409);
    EXPECT_EQ(json::parse(second->body)["error"]["code"], "COMMISSION_BUSY");
    release = true;
    waitJobFinished(port, json::parse(first->body)["jobId"]);
}

// 종료(취소 요청) 뒤 접수는 E2와 같이 502
TEST_F(HttpServerTest, E3AfterCancelRequestIs502) {
    commissioning.requestCancel();
    auto response = post("/api/commission", R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":"10.0.0.1"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 502);
    EXPECT_EQ(json::parse(response->body)["error"]["code"], "DEVICE_UNREACHABLE");
    EXPECT_TRUE(tcpFake.commissionRequests.empty());
}

// Q13: 진행 중(RUNNING) E8 본문, 실패 코드 본문(COMMISSION_FAILED·CANCELLED), 1시간 뒤 404
TEST_F(HttpServerTest, E8BodiesWhileRunningAndForFailuresAndAfterRetention) {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    tcpFake.onCommission = [&] {
        entered = true;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    tcpFake.commissionResult = Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "ble");
    auto accepted = post("/api/commission", R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":"10.0.0.1"})");
    ASSERT_TRUE(accepted);
    const std::string jobId = json::parse(accepted->body)["jobId"];
    while (!entered) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    httplib::Client client("127.0.0.1", port);
    auto running = client.Get("/api/commission/" + jobId, authHeaders());
    ASSERT_TRUE(running);
    EXPECT_EQ(json::parse(running->body),
              json::parse(R"({"jobId":")" + jobId + R"(","state":"RUNNING","deviceId":null,"restored":false,"error":null})"));
    release = true;
    EXPECT_EQ(waitJobFinished(port, jobId)["error"],
              json::parse(R"({"code":"COMMISSION_FAILED","message":"기기 등록에 실패했습니다."})"));

    tcpFake.onCommission = nullptr;
    tcpFake.commissionResult = Result<CommissionOutcome>::failure(ErrorCode::Cancelled, "stop");
    auto cancelled = post("/api/commission", R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":"10.0.0.1"})");
    ASSERT_TRUE(cancelled);
    const std::string cancelledId = json::parse(cancelled->body)["jobId"];
    EXPECT_EQ(waitJobFinished(port, cancelledId)["error"],
              json::parse(R"({"code":"CANCELLED","message":"서버 종료로 취소되었습니다."})"));

    clock.advance(CommissioningService::kFinishedJobRetention);
    auto expired = client.Get("/api/commission/" + cancelledId, authHeaders());
    ASSERT_TRUE(expired);
    EXPECT_EQ(expired->status, 404);
    EXPECT_EQ(json::parse(expired->body)["error"]["code"], "JOB_NOT_FOUND");
}

// Q10: E3 202·400·409 어느 경우에도 요청 비밀값(비밀번호·SSID·설정 코드)이 로그에 없다
class HttpServerLogTest : public HttpServerTest {
protected:
    void SetUp() override {
        capture.start();  // 서버·작업 스레드를 띄우기 전에
        HttpServerTest::SetUp();
    }
    void TearDown() override {
        HttpServerTest::TearDown();
        commissioning.stop();  // 출력하는 스레드를 모두 멈춘 뒤 되돌린다
        capture.stop();
    }
    LogCapture capture;
};

TEST_F(HttpServerLogTest, E3ResponsesNeverLogRequestSecrets) {
    const std::string body = R"({"protocolType":"MATTER","deviceName":"plug","payload":"34970112332","ssid":"MyHomeSSID","password":"pw-SECRET-123"})";
    std::atomic<bool> release{false};
    matterFake.onCommission = [&] {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    auto accepted = post("/api/commission", body);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->status, 202);
    auto busy = post("/api/commission", body);
    ASSERT_TRUE(busy);
    EXPECT_EQ(busy->status, 409);
    auto invalid = post("/api/commission", R"({"protocolType":"MATTER","deviceName":"plug","payload":"bad-pw-SECRET-123","password":"pw-SECRET-123"})");
    ASSERT_TRUE(invalid);
    EXPECT_EQ(invalid->status, 400);
    release = true;
    waitJobFinished(port, json::parse(accepted->body)["jobId"]);

    const std::string logs = capture.text();
    EXPECT_NE(logs.find("기기 등록(POST) 요청 수신"), std::string::npos);
    for (const char* secret : {"pw-SECRET-123", "MyHomeSSID", "34970112332"}) {
        EXPECT_EQ(logs.find(secret), std::string::npos) << secret;
    }
}

// 빈 payload: 루프백으로 접속하면 기기가 닿을 주소가 아니라 400, LAN 주소로 접속하면 그 주소를 기기에 보낸다
TEST_F(HttpServerTest, E3EmptyTcpPayloadUsesAddressTheAppConnectedTo) {
    auto loopback = post("/api/commission", R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":""})");
    ASSERT_TRUE(loopback);
    EXPECT_EQ(loopback->status, 400);
    EXPECT_EQ(json::parse(loopback->body), json::parse(kInvalidRequestBody));
    EXPECT_TRUE(tcpFake.commissionRequests.empty());

    const std::string lanAddress = firstLanAddressOfThisHost();
    if (lanAddress.empty()) GTEST_SKIP() << "이 기기에 사설 IPv4가 없어 LAN 경로는 건너뜀";
    httplib::Client client(lanAddress, port);
    auto response = client.Post("/api/commission", authHeaders(), R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":""})",
                                "application/json");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 202);
    waitJobFinished(port, json::parse(response->body)["jobId"]);
    ASSERT_EQ(tcpFake.commissionRequests.size(), 1u);
    EXPECT_EQ(tcpFake.commissionRequests[0].payload, lanAddress);
}

TEST_F(HttpServerTest, E3ValidationErrorsReturn400EnvelopeWithoutCommissioning) {
    const std::string longName(65, 'a');
    const std::vector<std::string> bodies = {
        "not json", "{}", "[]",
        R"({"protocolType":"ZIGBEE","deviceName":"x","payload":""})",
        R"({"protocolType":"MATTER","deviceName":"x","payload":"abc"})",
        R"({"protocolType":"TCP_DIY","deviceName":"","payload":""})",
        R"({"protocolType":"TCP_DIY","deviceName":")" + longName + R"(","payload":""})",
        R"({"protocolType":"TCP_DIY","deviceName":"a\nb","payload":""})",
        R"({"protocolType":"TCP_DIY","deviceName":"x","payload":"not-an-ip"})",
        R"({"protocolType":"TCP_DIY","deviceName":"x","payload":"","ssid":null})",
        R"({"protocolType":"TCP_DIY","deviceName":"x","payload":"","driverTypes":[99]})"};
    for (const std::string& body : bodies) {
        auto response = post("/api/commission", body);
        ASSERT_TRUE(response) << body;
        EXPECT_EQ(response->status, 400) << body;
        EXPECT_EQ(json::parse(response->body), json::parse(kInvalidRequestBody)) << body;
    }
    EXPECT_TRUE(tcpFake.commissionRequests.empty());
}

// ---- E4 ----

// 가짜 시계가 1970-01-01 00:00:00 UTC에 있으므로 숨김 시각과 복구 기한(+90일)이 고정된다
TEST_F(HttpServerTest, E4DeleteReturnsHiddenBodyThenMissingReturns404) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    auto ok = post("/api/devices/delete", R"({"deviceId":"FAKE_1"})");
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
    EXPECT_EQ(json::parse(ok->body), json::parse(R"({"deviceId":"FAKE_1","deletedAt":"1970-01-01T00:00:00Z",
        "restorableUntil":"1970-04-01T00:00:00Z","unpaired":true})"));

    auto missing = post("/api/devices/delete", R"({"deviceId":"FAKE_1"})");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
    EXPECT_EQ(json::parse(missing->body), json::parse(kDeviceNotFoundBody));
}

TEST_F(HttpServerTest, E4HiddenDeviceIsGoneFromListAndOtherEndpoints) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    ASSERT_TRUE(post("/api/devices/delete", R"({"deviceId":"PLUG_1"})"));
    httplib::Client client("127.0.0.1", port);

    for (const std::string suffix : {"/state", "/energy", "/energy/history"}) {
        auto response = client.Get("/api/devices/PLUG_1" + suffix, authHeaders());
        ASSERT_TRUE(response) << suffix;
        EXPECT_EQ(response->status, 404) << suffix;
        EXPECT_EQ(json::parse(response->body), json::parse(kDeviceNotFoundBody)) << suffix;
    }
}

TEST_F(HttpServerTest, E4InvalidJsonReturns400Envelope) {
    for (const std::string body : {"nope", "{}", "[]", R"({"deviceId":5})"}) {
        auto response = post("/api/devices/delete", body);
        ASSERT_TRUE(response) << body;
        EXPECT_EQ(response->status, 400) << body;
        EXPECT_EQ(json::parse(response->body), json::parse(kInvalidRequestBody)) << body;
    }
}

// 오프라인 TCP 기기(연결 없음 → 해제 실패)도 숨김은 성공이고 unpaired만 false다
TEST_F(HttpServerTest, E4DeleteOfflineDeviceStillSucceedsWithUnpairedFalse) {
    addDevice(ProtocolType::TCP_DIY, "ARD_9", "switch");
    tcpFake.unpairResult = false;

    auto res = post("/api/devices/delete", R"({"deviceId":"ARD_9"})");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(json::parse(res->body)["unpaired"], false);
    Device device;
    EXPECT_FALSE(manager.getDevice("ARD_9", device));
}

// ---- 종료 중 기기 호출은 200이 아니라 502 ----

TEST_F(HttpServerTest, DuringShutdownE2ImmediateCommandReturns502WithoutTouchingDevice) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    manager.beginShutdown();
    auto response = httplib::Client("127.0.0.1", port).Post("/api/control", authHeaders(),
                                                           R"({"deviceId":"FAKE_1","command":"ON"})", "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 502);
    EXPECT_TRUE(tcpFake.sentCommands.empty());
}

TEST_F(HttpServerTest, DuringShutdownE5CancelledStateReturns502) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    matterFake.stateToReturn = "UNKNOWN"; // 취소된 Matter 조회가 돌려주는 값
    manager.beginShutdown();
    auto response = httplib::Client("127.0.0.1", port).Get("/api/devices/PLUG_1/state?isManualRefresh=true", authHeaders());
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 502);
}

TEST_F(HttpServerTest, DuringShutdownE6CancelledEnergyReturns502) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    matterFake.energyResult = Result<EnergyReading>::failure(ErrorCode::Cancelled, "cancelled");
    manager.beginShutdown();
    auto response = httplib::Client("127.0.0.1", port).Get("/api/devices/PLUG_1/energy", authHeaders());
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 502);
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

TEST_F(HttpServerTest, E5ForwardsIsManualRefreshOnlyWhenTrue) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    httplib::Client client("127.0.0.1", port);
    ASSERT_TRUE(client.Get("/api/devices/FAKE_1/state", authHeaders()));
    ASSERT_TRUE(client.Get("/api/devices/FAKE_1/state?isManualRefresh=true", authHeaders()));
    ASSERT_TRUE(client.Get("/api/devices/FAKE_1/state?isManualRefresh=1", authHeaders()));
    EXPECT_EQ(tcpFake.readStateManualFlags, (std::vector<bool>{false, true, false}));
}

TEST_F(HttpServerTest, E5UnresponsiveDriverReturns200Offline) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    tcpFake.stateToReturn = "OFFLINE";
    auto response = httplib::Client("127.0.0.1", port).Get("/api/devices/FAKE_1/state", authHeaders());
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"deviceId":"FAKE_1","state":"OFFLINE"})"));
}

// 조회 도중 기기가 삭제되면 404이거나, 읽은 실제 상태로 200이다. 200 UNKNOWN은 아니다
TEST_F(HttpServerTest, E5DeviceRemovedDuringReadIsNeverReportedAsUnknown) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    tcpFake.stateToReturn = "ON";
    tcpFake.onReadState = [this](const std::string& id) { manager.removeDevice(id); };
    auto response = httplib::Client("127.0.0.1", port).Get("/api/devices/FAKE_1/state", authHeaders());
    ASSERT_TRUE(response);
    if (response->status == 200) {
        EXPECT_EQ(json::parse(response->body)["state"], "ON");
    } else {
        EXPECT_EQ(response->status, 404);
    }
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

TEST_F(HttpServerTest, E6ReadFailureReturns502ForUnreachable) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    matterFake.energyResult = Result<EnergyReading>::failure(ErrorCode::DeviceUnreachable, "read failed");
    auto failed = httplib::Client("127.0.0.1", port).Get("/api/devices/PLUG_1/energy", authHeaders());
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->status, 502);
    EXPECT_EQ(json::parse(failed->body),
              json::parse(R"({"error":{"code":"DEVICE_UNREACHABLE","message":"기기가 응답하지 않습니다."}})"));
}

// 기기 출력 해석 실패(숫자 아님)도 파서가 DeviceUnreachable로 정해 502가 된다
TEST_F(HttpServerTest, E6UnparsableDeviceOutputReturns502) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    matterFake.energyResult = ChipToolOutputParser::parseEnergyReading("abc", "100");
    auto failed = httplib::Client("127.0.0.1", port).Get("/api/devices/PLUG_1/energy", authHeaders());
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->status, 502);
}

// ---- E7 ----

TEST_F(HttpServerTest, E7RawHistoryReturnsIsoLogsAndTruncatedFlag) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    database.insertEnergyLog("PLUG_1", 100, "2026-11-16 00:20:00");
    database.insertEnergyLog("PLUG_1", 200, "2026-11-16 00:21:00");
    database.insertEnergyLog("PLUG_1", 300, "2026-11-16 00:22:00");
    httplib::Client client("127.0.0.1", port);

    auto all = client.Get("/api/devices/PLUG_1/energy/history", authHeaders());
    ASSERT_TRUE(all);
    EXPECT_EQ(all->status, 200);
    EXPECT_EQ(json::parse(all->body), json::parse(R"({"deviceId":"PLUG_1","state":"UNKNOWN","logs":[
        {"timestamp":"2026-11-16T00:20:00Z","totalEnergy_mWh":100},
        {"timestamp":"2026-11-16T00:21:00Z","totalEnergy_mWh":200},
        {"timestamp":"2026-11-16T00:22:00Z","totalEnergy_mWh":300}],"truncated":false})"));

    auto cut = client.Get("/api/devices/PLUG_1/energy/history?limit=2", authHeaders());
    ASSERT_TRUE(cut);
    EXPECT_EQ(json::parse(cut->body), json::parse(R"({"deviceId":"PLUG_1","state":"UNKNOWN","logs":[
        {"timestamp":"2026-11-16T00:21:00Z","totalEnergy_mWh":200},
        {"timestamp":"2026-11-16T00:22:00Z","totalEnergy_mWh":300}],"truncated":true})"));

    auto exact = client.Get("/api/devices/PLUG_1/energy/history?limit=3", authHeaders());
    ASSERT_TRUE(exact);
    EXPECT_EQ(json::parse(exact->body)["truncated"], false);
}

TEST_F(HttpServerTest, E7UnknownDeviceReturns404AndTcpDeviceReturns400) {
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    httplib::Client client("127.0.0.1", port);

    auto missing = client.Get("/api/devices/NOPE/energy/history", authHeaders());
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
    EXPECT_EQ(json::parse(missing->body), json::parse(kDeviceNotFoundBody));

    auto tcp = client.Get("/api/devices/FAKE_1/energy/history", authHeaders());
    ASSERT_TRUE(tcp);
    EXPECT_EQ(tcp->status, 400);
    EXPECT_EQ(json::parse(tcp->body),
              json::parse(R"({"error":{"code":"UNSUPPORTED_OPERATION","message":"이 기기에서 지원하지 않는 요청입니다."}})"));
}

// 계약 예시(2026-11-01 일요일 ~ 11-17, week)를 분 단위 기록으로 그대로 만든다
TEST_F(HttpServerTest, E7WeekAggregationReproducesContractExample) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    const auto localMidnight = *UtcTime::fromDbText("2026-10-31 15:00:00"); // 현지 11-01 00:00
    ASSERT_TRUE(database.insertEnergyLog("PLUG_1", 0, UtcTime::toDbText(localMidnight - std::chrono::minutes(1))));
    struct Segment { int samples; int64_t base; int64_t step; int64_t usage; };
    const Segment segments[] = {{1440, 0, 1, 1200}, {10080, 1200, 1, 9100}, {10080, 10300, 1, 8800}, {2100, 19100, 2, 2500}};
    int minute = 0;
    for (const auto& segment : segments) {
        for (int i = 0; i < segment.samples; ++i, ++minute) {
            const int64_t total = segment.base + std::min<int64_t>(segment.step * (i + 1), segment.usage);
            ASSERT_TRUE(database.insertEnergyLog("PLUG_1", total, UtcTime::toDbText(localMidnight + std::chrono::minutes(minute))));
        }
    }

    auto response = httplib::Client("127.0.0.1", port).Get(
        "/api/devices/PLUG_1/energy/history?from=2026-11-01&to=2026-11-17&period=week", authHeaders());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"deviceId":"PLUG_1","state":"UNKNOWN","period":"week","utcOffset":"+09:00",
      "buckets":[
        {"start":"2026-11-01","end":"2026-11-01","energyUsed_mWh":1200,"sampleCount":1440},
        {"start":"2026-11-02","end":"2026-11-08","energyUsed_mWh":9100,"sampleCount":10080},
        {"start":"2026-11-09","end":"2026-11-15","energyUsed_mWh":8800,"sampleCount":10080},
        {"start":"2026-11-16","end":"2026-11-17","energyUsed_mWh":2500,"sampleCount":2100}]})"));
}

TEST_F(HttpServerTest, E7MonthAggregationIncludesEmptyBuckets) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    httplib::Client client("127.0.0.1", port);

    auto response = client.Get("/api/devices/PLUG_1/energy/history?from=2028-02-15&to=2028-03-02&period=month", authHeaders());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body)["buckets"], json::parse(R"([
        {"start":"2028-02-15","end":"2028-02-29","energyUsed_mWh":0,"sampleCount":0},
        {"start":"2028-03-01","end":"2028-03-02","energyUsed_mWh":0,"sampleCount":0}])"));
}

TEST_F(HttpServerTest, E7LimitQueryIsAppliedAndInvalidQueriesReturn400) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    for (int i = 1; i <= 5; ++i) database.insertEnergyLog("PLUG_1", i * 100, "2026-11-19 00:00:00");
    httplib::Client client("127.0.0.1", port);

    auto limited = client.Get("/api/devices/PLUG_1/energy/history?limit=2", authHeaders());
    ASSERT_TRUE(limited);
    EXPECT_EQ(limited->status, 200);
    // 최근 2건을 오래된 순으로
    const json limitedBody = json::parse(limited->body);
    EXPECT_EQ(limitedBody["truncated"], true);
    const json logs = limitedBody["logs"];
    ASSERT_EQ(logs.size(), 2u);
    EXPECT_EQ(logs[0]["totalEnergy_mWh"], 400);
    EXPECT_EQ(logs[1]["totalEnergy_mWh"], 500);

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

TEST_F(HttpServerTest, E4HiddenDeviceIsAbsentFromListButOthersRemain) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    addDevice(ProtocolType::TCP_DIY, "FAKE_1", "lamp");
    ASSERT_TRUE(post("/api/devices/delete", R"({"deviceId":"PLUG_1"})"));

    auto list = httplib::Client("127.0.0.1", port).Get("/api/devices", authHeaders());

    ASSERT_TRUE(list);
    EXPECT_EQ(list->status, 200);
    const std::string body = list->body;
    EXPECT_EQ(body.find("PLUG_1"), std::string::npos);
    EXPECT_NE(body.find("FAKE_1"), std::string::npos);
}

TEST_F(HttpServerTest, E7RawHistoryFiltersByLocalDateRange) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    // 현지 정오(= UTC 03:00)에 하루 한 건
    database.insertEnergyLog("PLUG_1", 100, "2026-11-15 03:00:00");
    database.insertEnergyLog("PLUG_1", 200, "2026-11-16 03:00:00");
    database.insertEnergyLog("PLUG_1", 300, "2026-11-17 03:00:00");
    httplib::Client client("127.0.0.1", port);

    const auto totals = [&](const std::string& query) {
        auto response = client.Get("/api/devices/PLUG_1/energy/history?" + query, authHeaders());
        std::vector<int64_t> result;
        if (!response || response->status != 200) return result;
        const json body = json::parse(response->body);
        for (const auto& log : body["logs"]) result.push_back(log["totalEnergy_mWh"]);
        return result;
    };

    EXPECT_EQ(totals("from=2026-11-16&to=2026-11-16"), (std::vector<int64_t>{200}));
    EXPECT_EQ(totals("from=2026-11-16"), (std::vector<int64_t>{200, 300}));
    EXPECT_EQ(totals("to=2026-11-15"), (std::vector<int64_t>{100}));
}

TEST_F(HttpServerTest, E7DayAggregationIgnoresLimit) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    database.insertEnergyLog("PLUG_1", 100, "2026-11-16 03:00:00");
    database.insertEnergyLog("PLUG_1", 160, "2026-11-17 03:00:00");

    auto response = httplib::Client("127.0.0.1", port).Get(
        "/api/devices/PLUG_1/energy/history?from=2026-11-16&to=2026-11-18&period=day&limit=1", authHeaders());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body)["buckets"], json::parse(R"([
        {"start":"2026-11-16","end":"2026-11-16","energyUsed_mWh":0,"sampleCount":1},
        {"start":"2026-11-17","end":"2026-11-17","energyUsed_mWh":60,"sampleCount":1},
        {"start":"2026-11-18","end":"2026-11-18","energyUsed_mWh":0,"sampleCount":0}])"));
}

// 한도(1000)를 넘는 limit은 거절하지 않고 1000건으로 줄인다
TEST_F(HttpServerTest, E7LimitAboveMaximumIsClampedTo1000) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    for (int i = 0; i < 1005; ++i) {
        ASSERT_TRUE(database.insertEnergyLog("PLUG_1", i, "2026-11-19 00:00:00"));
    }

    auto response = httplib::Client("127.0.0.1", port).Get("/api/devices/PLUG_1/energy/history?limit=5000", authHeaders());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const json body = json::parse(response->body);
    EXPECT_EQ(body["logs"].size(), 1000u);
    EXPECT_EQ(body["truncated"], true);
}

TEST_F(HttpServerTest, E7RangeOf367DaysAndOutOfRangeYearsReturn400) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    httplib::Client client("127.0.0.1", port);
    for (const std::string query : {"from=2026-01-01&to=2027-01-02&period=day", "from=2300-01-01&to=2300-01-02&period=day",
                                    "from=1999-12-31&to=2000-01-02&period=day", "from=2101-01-01", "to=1600-01-01"}) {
        auto response = client.Get("/api/devices/PLUG_1/energy/history?" + query, authHeaders());
        ASSERT_TRUE(response) << query;
        EXPECT_EQ(response->status, 400) << query;
        EXPECT_EQ(json::parse(response->body), json::parse(kInvalidRequestBody)) << query;
    }
    auto boundary = client.Get("/api/devices/PLUG_1/energy/history?from=2000-01-01&to=2000-01-02&period=day", authHeaders());
    ASSERT_TRUE(boundary);
    EXPECT_EQ(boundary->status, 200);
}

// 누적계가 줄어들면(기기 카운터 초기화) 그 차이는 0으로 보고 다음 증가부터 센다
TEST_F(HttpServerTest, E7CounterResetCountsAsZeroUsage) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    database.insertEnergyLog("PLUG_1", 100, "2026-11-16 01:00:00");
    database.insertEnergyLog("PLUG_1", 150, "2026-11-16 02:00:00");
    database.insertEnergyLog("PLUG_1", 20, "2026-11-16 03:00:00");
    database.insertEnergyLog("PLUG_1", 70, "2026-11-16 04:00:00");

    auto response = httplib::Client("127.0.0.1", port).Get(
        "/api/devices/PLUG_1/energy/history?from=2026-11-16&to=2026-11-16&period=day", authHeaders());

    ASSERT_TRUE(response);
    EXPECT_EQ(json::parse(response->body)["buckets"], json::parse(R"([
        {"start":"2026-11-16","end":"2026-11-16","energyUsed_mWh":100,"sampleCount":4}])"));
}

// 기록이 없는 날은 0건·0으로 남고, 긴 공백 뒤 첫 기록의 증가분은 그 기록이 속한 날에 붙는다
TEST_F(HttpServerTest, E7GapDaysAreEmptyAndUsageLandsOnLaterDay) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    database.insertEnergyLog("PLUG_1", 100, "2026-11-16 03:00:00");
    database.insertEnergyLog("PLUG_1", 400, "2026-11-19 03:00:00");

    auto response = httplib::Client("127.0.0.1", port).Get(
        "/api/devices/PLUG_1/energy/history?from=2026-11-16&to=2026-11-19&period=day", authHeaders());

    ASSERT_TRUE(response);
    EXPECT_EQ(json::parse(response->body)["buckets"], json::parse(R"([
        {"start":"2026-11-16","end":"2026-11-16","energyUsed_mWh":0,"sampleCount":1},
        {"start":"2026-11-17","end":"2026-11-17","energyUsed_mWh":0,"sampleCount":0},
        {"start":"2026-11-18","end":"2026-11-18","energyUsed_mWh":0,"sampleCount":0},
        {"start":"2026-11-19","end":"2026-11-19","energyUsed_mWh":300,"sampleCount":1}])"));
}

}  // namespace
