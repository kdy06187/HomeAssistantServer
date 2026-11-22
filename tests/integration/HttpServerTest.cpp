#include <gtest/gtest.h>
#include "DatabaseManager.hpp"
#include "support/HistoryTestHelper.hpp"
#include "DeviceManager.hpp"
#include "DriverManager.hpp"
#include "HTTPServer.hpp"
#include "fakes/FakeClock.hpp"
#include "fakes/FakeProtocolDriver.hpp"
#include "ChipToolOutputParser.hpp"
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
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
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
                  {"deviceId":"FAKE_1","name":"lamp","protocolType":"TCP_DIY","state":"UNKNOWN"}]})"));
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
    bareManager.addDevice("BARE_1", "bare", ProtocolType::TCP_DIY, {}, nullptr);
    const int barePort = findFreePort();
    HTTPServer bareServer(bareManager, kToken, barePort);
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
    FakeProtocolDriver tcpFake;
    ASSERT_TRUE(database.init(":memory:"));
    manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [&tcpFake](IMessageSender*) {
        return std::make_shared<PCDriver>(&tcpFake);
    });
    manager.addDevice("FAKE_1", "pc", ProtocolType::TCP_DIY, {DriverType::PC_DRIVER}, &tcpFake);
    manager.updateDeviceState("FAKE_1", "ON");
    const int serverPort = findFreePort();
    HTTPServer server(manager, kToken, serverPort);
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
    FakeProtocolDriver tcpFake;
    ASSERT_TRUE(database.init(":memory:"));
    manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [&tcpFake](IMessageSender*) {
        return std::make_shared<PCDriver>(&tcpFake);
    });
    manager.addDevice("FAKE_1", "pc", ProtocolType::TCP_DIY, {DriverType::PC_DRIVER}, &tcpFake);
    manager.updateDeviceState("FAKE_1", "ON");
    const int serverPort = findFreePort();
    HTTPServer server(manager, kToken, serverPort);
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
    EXPECT_EQ(tcpFake.broadcastMessages.size(), 1u);
}

// 종료 중 취소된 기기 호출이 200(성공 상태 코드)으로 나가지 않고 502로 나간다
TEST(HttpServerDelayedCommandTest, E2DuringShutdownReturns502NotCancelled200) {
    DatabaseManager database;
    DriverManager deviceDrivers;
    FakeClock clock;
    DelayedCommandScheduler scheduler{clock};
    DeviceManager manager{database, deviceDrivers, scheduler, clock};
    FakeProtocolDriver tcpFake;
    ASSERT_TRUE(database.init(":memory:"));
    manager.registerDriver(ProtocolType::TCP_DIY, &tcpFake);
    deviceDrivers.registerDriver(DriverType::PC_DRIVER, [&tcpFake](IMessageSender*) {
        return std::make_shared<PCDriver>(&tcpFake);
    });
    manager.addDevice("FAKE_1", "pc", ProtocolType::TCP_DIY, {DriverType::PC_DRIVER}, &tcpFake);
    manager.updateDeviceState("FAKE_1", "ON");
    manager.beginShutdown();
    scheduler.stopAndCancelAll();
    const int serverPort = findFreePort();
    HTTPServer server(manager, kToken, serverPort);
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
    EXPECT_TRUE(tcpFake.broadcastMessages.empty());
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

// 성공·등록 실패 본문은 v1.0 그대로(13번에서 비동기로 바뀜). 입력 검증 오류만 새 오류 봉투
TEST_F(HttpServerTest, E3CommissionSuccess) {
    auto response = post("/api/commission",
        R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":"10.0.0.1","ssid":"s","password":"p"})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"result":"success","message":"기기 등록 성공!"})"));
    ASSERT_EQ(tcpFake.commissionRequests.size(), 1u);
    EXPECT_EQ(tcpFake.commissionRequests[0].name, "lamp");
    EXPECT_EQ(tcpFake.commissionRequests[0].ssid, "s");
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
        EXPECT_EQ(response->status, 200) << payload;
        EXPECT_EQ(matterFake.commissionRequests.back().payload, payload);
        Device device;
        ASSERT_TRUE(manager.getDevice("PLUG_" + std::to_string(index), device));
        EXPECT_EQ(device.driver_types, std::vector<DriverType>{DriverType::PC_DRIVER});
    }
}

TEST_F(HttpServerTest, E3CommissionFailureReturns400WithLegacyBody) {
    tcpFake.commissionResult = Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "fail");
    auto response = post("/api/commission", R"({"protocolType":"TCP_DIY","deviceName":"lamp","payload":""})");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    EXPECT_EQ(json::parse(response->body), json::parse(R"({"result":"error","message":"기기 등록 실패!"})"));
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

TEST_F(HttpServerTest, E4DeleteSuccessAndMissingKeepLegacyBodies) {
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
}

TEST_F(HttpServerTest, E4InvalidJsonReturns400Envelope) {
    for (const std::string body : {"nope", "{}", "[]", R"({"deviceId":5})"}) {
        auto response = post("/api/devices/delete", body);
        ASSERT_TRUE(response) << body;
        EXPECT_EQ(response->status, 400) << body;
        EXPECT_EQ(json::parse(response->body), json::parse(kInvalidRequestBody)) << body;
    }
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

// from·to·period와 v2.0 키·시각 형식은 12번에서 바꾼다. 지금은 v1.0 본문 + 새 오류 규약
TEST_F(HttpServerTest, E7HistoryReturnsLogsAnd404ForUnknownDevice) {
    addDevice(ProtocolType::MATTER, "PLUG_1", "plug");
    database.insertEnergyLog("PLUG_1", 100, "2026-11-19 00:00:00");
    database.insertEnergyLog("PLUG_1", 200, "2026-11-19 00:00:00");
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
    for (int i = 1; i <= 5; ++i) database.insertEnergyLog("PLUG_1", i * 100, "2026-11-19 00:00:00");
    httplib::Client client("127.0.0.1", port);

    auto limited = client.Get("/api/devices/PLUG_1/energy/history?limit=2", authHeaders());
    ASSERT_TRUE(limited);
    EXPECT_EQ(limited->status, 200);
    // 최근 2건을 오래된 순으로
    const json logs = json::parse(limited->body)["logs"];
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

}  // namespace
