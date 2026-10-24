#include "mcp/server.h"

#include <asio.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sv::mcp;

namespace {
Request post(const json &body, std::map<std::string, std::string> headers = {}) {
    headers.emplace("authorization", "Bearer secret");
    return {"POST", "/mcp", std::move(headers), body.dump()};
}
json rpc(const std::string &method, json params = json::object(), int id = 1) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

struct Fixture {
    Server server;
    bool debug = false;
    Fixture() {
        server.set_info("test", "1.0", "hello");
        server.set_tools({
            {"echo",
             "Echo",
             {{"type", "object"}, {"required", {"a"}}},
             true,
             [](const json &a) { return text_result(a); },
             {}},
            {"boom",
             "Throws",
             {{"type", "object"}},
             false,
             [](const json &) -> json { throw std::runtime_error("kaput"); },
             {}},
            {"secret",
             "Debug only",
             {{"type", "object"}},
             true,
             [](const json &) { return text_result("x"); },
             [this] { return debug; }},
        });
        server.set_resources({{"visualiser://project.json", "Project", "application/json", [] { return "{}"; }}});
        server.start({"127.0.0.1", 0, "secret"});
    }
    json call(const json &message, std::map<std::string, std::string> headers = {}) {
        const Response r = server.handle(post(message, std::move(headers)));
        REQUIRE(r.status == 200);
        return json::parse(r.body);
    }
};
} // namespace

TEST_CASE("MCP handshake gives a session and echoes a supported version", "[mcp]") {
    Fixture f;
    const Response r = f.server.handle(post(rpc("initialize", {{"protocolVersion", "2025-06-18"}})));
    REQUIRE(r.status == 200);
    REQUIRE(r.headers.contains("Mcp-Session-Id"));
    const json body = json::parse(r.body);
    CHECK(body["result"]["protocolVersion"] == "2025-06-18");
    CHECK(body["result"]["serverInfo"]["name"] == "test");
    CHECK(body["result"]["instructions"] == "hello");
    CHECK(f.call(rpc("initialize", {{"protocolVersion", "1999-01-01"}}))["result"]["protocolVersion"] == "2025-11-25");

    const std::string session = r.headers.at("Mcp-Session-Id");
    CHECK(f.call(rpc("ping"), {{"mcp-session-id", session}})["result"] == json::object());
    CHECK(f.server.handle(post(rpc("ping"), {{"mcp-session-id", "nope"}})).status == 404);
    CHECK(f.server.handle({"DELETE", "/mcp", {{"authorization", "Bearer secret"}, {"mcp-session-id", session}}, {}})
              .status == 200);
    CHECK(f.server.handle(post(rpc("ping"), {{"mcp-session-id", session}})).status == 404);
}

TEST_CASE("MCP rejects bad token, origin, method, version and batches", "[mcp]") {
    Fixture f;
    CHECK(f.server.handle({"POST", "/mcp", {}, rpc("ping").dump()}).status == 401);
    CHECK(f.server.handle({"POST", "/mcp", {{"authorization", "Bearer secreT"}}, rpc("ping").dump()}).status == 401);
    CHECK(f.server.handle(post(rpc("ping"), {{"origin", "http://evil.example"}})).status == 403);
    CHECK(f.server.handle(post(rpc("ping"), {{"origin", "http://localhost:5173"}})).status == 200);
    CHECK(f.server.handle({"GET", "/mcp", {{"authorization", "Bearer secret"}}, {}}).status == 405);
    CHECK(f.server.handle(post(rpc("ping"), {{"mcp-protocol-version", "2020-01-01"}})).status == 400);
    CHECK(f.server.handle({"POST", "/other", {{"authorization", "Bearer secret"}}, {}}).status == 404);
    const Response batch = f.server.handle(post(json::array({rpc("ping")})));
    CHECK(batch.status == 400);
    CHECK(json::parse(batch.body)["error"]["code"] == -32600);
    const Response bad = f.server.handle({"POST", "/mcp", {{"authorization", "Bearer secret"}}, "{nope"});
    CHECK(json::parse(bad.body)["error"]["code"] == -32700);
    CHECK(f.server.handle(post({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})).status == 202);
}

TEST_CASE("MCP tools list, call, errors and availability", "[mcp]") {
    Fixture f;
    json tools = f.call(rpc("tools/list"))["result"]["tools"];
    CHECK(tools.size() == 2);
    CHECK(tools[0]["annotations"]["readOnlyHint"] == true);

    const json echo = f.call(rpc("tools/call", {{"name", "echo"}, {"arguments", {{"a", 1}}}}))["result"];
    CHECK(echo["structuredContent"]["a"] == 1);
    const json missing = f.call(rpc("tools/call", {{"name", "echo"}, {"arguments", {{"b", 1}}}}))["result"];
    CHECK(missing["isError"] == true);
    CHECK(missing["content"][0]["text"] == "Missing argument: a");
    const json boom = f.call(rpc("tools/call", {{"name", "boom"}}))["result"];
    CHECK(boom["isError"] == true);
    CHECK(boom["content"][0]["text"] == "kaput");
    CHECK(f.call(rpc("tools/call", {{"name", "secret"}}))["error"]["code"] == -32602);
    CHECK(f.call(rpc("nope"))["error"]["code"] == -32601);

    f.debug = true;
    CHECK(f.call(rpc("tools/list"))["result"]["tools"].size() == 3);
    CHECK(f.call(rpc("tools/call", {{"name", "secret"}}))["result"]["content"][0]["text"] == "x");
    CHECK(f.server.status().calls == 4);
    CHECK(f.server.status().last_tool == "secret");

    CHECK(f.call(rpc("resources/list"))["result"]["resources"][0]["uri"] == "visualiser://project.json");
    CHECK(f.call(rpc("resources/read", {{"uri", "visualiser://project.json"}}))["result"]["contents"][0]["text"] ==
          "{}");
    CHECK(f.call(rpc("resources/read", {{"uri", "x://y"}}))["error"]["code"] == -32002);
}

TEST_CASE("MCP answers over a real socket with keep-alive", "[mcp]") {
    Fixture f;
    const auto port = f.server.status().port;
    REQUIRE(port != 0);
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    const auto exchange = [&](const std::string &body) {
        const std::string req = "POST /mcp HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer secret\r\nContent-Type: "
                                "application/json\r\nContent-Length: " +
                                std::to_string(body.size()) + "\r\n\r\n" + body;
        asio::write(socket, asio::buffer(req));
        asio::streambuf buf;
        const std::size_t n = asio::read_until(socket, buf, "\r\n\r\n");
        std::string head(asio::buffers_begin(buf.data()), asio::buffers_begin(buf.data()) + std::ptrdiff_t(n));
        buf.consume(n);
        const auto at = head.find("Content-Length: ") + 16;
        const std::size_t length = std::stoul(head.substr(at, head.find("\r\n", at) - at));
        if (buf.size() < length)
            asio::read(socket, buf, asio::transfer_exactly(length - buf.size()));
        return std::pair{head, std::string(asio::buffers_begin(buf.data()),
                                           asio::buffers_begin(buf.data()) + std::ptrdiff_t(length))};
    };
    auto [head, body] = exchange(rpc("initialize").dump());
    CHECK(head.starts_with("HTTP/1.1 200"));
    CHECK(json::parse(body)["result"]["serverInfo"]["name"] == "test");
    std::tie(head, body) = exchange(
        rpc("tools/call", {{"name", "echo"}, {"arguments", {{"a", 1}, {"big", std::string(100000, 'x')}}}}).dump());
    CHECK(json::parse(body)["result"]["structuredContent"]["big"].get<std::string>().size() == 100000);
    f.server.stop();
    CHECK_FALSE(f.server.status().listening);
}
